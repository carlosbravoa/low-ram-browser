"""Check that a confined instance opens its window on the X11 display.

  python3 test_confined_window_x11.py <dir with lrb and lrb_coordinator>

Opens a window on the X11 display ($DISPLAY; XWayland works) for a few
seconds. The other X11 tests start lrb directly, unconfined; this one goes
through the coordinator, as users do. Landlock's abstract-socket scoping
once kept every confined instance off the X server ("Missing X server or
$DISPLAY"): libxcb tries X's abstract socket first and gives up on EPERM.
Local page only; settings in a temporary $XDG_CONFIG_HOME (GPU already
chosen, so the first-start question doesn't open).
"""

import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading

from lrb_harness import x11
from test_coordinator import instances, wait_for
from test_discard import LEAN_ARGS


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        page = b"<title>confined</title>confined"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(page)))
        self.end_headers()
        self.wfile.write(page)

    def log_message(self, *args):
        pass


def main():
    out = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="lrb-confined-x11-", dir="/tmp")
    profiles = os.path.join(work, "sites")
    os.makedirs(os.path.join(work, "config", "lrb"))
    with open(os.path.join(work, "config", "lrb", "settings.json"), "w") as f:
        f.write('{"gpu": false}\n')

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/"

    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    before = {w for w, _ in x11.windows()}
    log = open(os.path.join(work, "coordinator.log"), "w")
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:",
               XDG_CONFIG_HOME=os.path.join(work, "config"))
    coordinator = subprocess.Popen(
        [f"{out}/lrb_coordinator", f"--browser={out}/lrb",
         f"--profiles-dir={profiles}", f"--socket={work}/c.sock",
         "--adblock-setting=off", url, "--"] + [a for a in LEAN_ARGS
                                               if not a.startswith("--ozone-platform")],
        stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    try:
        site = wait_for(lambda: "127.0.0.1" in instances(profiles), 30)
        check("the page's instance started", bool(site))
        if site:
            pid = instances(profiles)["127.0.0.1"][0]
            with open(f"/proc/{pid}/status") as f:
                status = f.read()
            check("the instance is confined (seccomp, no_new_privs)",
                  "Seccomp:\t2" in status and "NoNewPrivs:\t1" in status)
        new = wait_for(lambda: [t for w, t in x11.windows() if w not in before], 20)
        check("its window opens on the X11 display", bool(new), f"new windows: {new}")
    finally:
        os.killpg(coordinator.pid, 15)
        coordinator.wait(10)
        server.shutdown()
        log.close()
    with open(os.path.join(work, "coordinator.log")) as f:
        text = f.read()
    check("no instance missed the X server", "Missing X server" not in text)
    if failures:
        print(f"kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
