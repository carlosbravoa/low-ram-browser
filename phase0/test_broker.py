"""Check uploads and downloads through the coordinator's file broker.

  python3 test_broker.py <dir with lrb and lrb_coordinator>

Instances run confined (lrb/coordinator/confine.h): they can't open the
user's files. The coordinator shows the picker and hands over only what
was chosen (lrb/coordinator/broker.h). Here a stand-in picker "chooses"
(the real one needs a desktop): an upload must reach the page as a copy,
a download must land where the picker said, and the instance must not be
able to read the original file itself. Local pages only.
"""

import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import run as runlib
from test_coordinator import devtools, instances, wait_for
from test_discard import LEAN_ARGS

PAGE = b"""<!doctype html><title>broker</title>
<input id=f type=file style="position:absolute;left:0;top:0;width:300px;height:80px">
<script>
f.addEventListener('change', async () => {
  window.lrbResult = f.files.length ? await f.files[0].text() : 'none';
});
f.addEventListener('cancel', () => window.lrbResult = 'cancel');
</script>"""
DOWNLOAD = b"downloaded through the broker\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/report":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="report.txt"')
            self.end_headers()
            self.wfile.write(DOWNLOAD)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, *args):
        pass


def main():
    out = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="broker-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    user = os.path.join(work, "user")
    os.makedirs(os.path.join(user, "saved"))
    original = os.path.join(user, "letter.txt")
    with open(original, "w") as f:
        f.write("the user's letter")
    picker = os.path.join(work, "picker")
    with open(picker, "w") as f:
        f.write("#!/bin/sh\n"
                "case \"$1\" in\n"
                f"  open) echo '{original}' ;;\n"
                "  save) for a; do case \"$a\" in --name=*) n=\"${a#--name=}\";; esac; done\n"
                f"        echo '{user}/saved/'\"$n\" ;;\n"
                "esac\n")
    os.chmod(picker, 0o755)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/"

    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    log = open(os.path.join(work, "coordinator.log"), "w")
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:")
    coordinator = subprocess.Popen(
        [f"{out}/lrb_coordinator", f"--browser={out}/lrb", f"--picker={picker}",
         f"--profiles-dir={profiles}", f"--socket={work}/c.sock",
         "--adblock-setting=off", url, "--"] + LEAN_ARGS,
        stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    try:
        site = wait_for(lambda: "127.0.0.1" in instances(profiles), 30)
        check("the page's instance started", bool(site))
        pid = instances(profiles)["127.0.0.1"][0]
        with open(f"/proc/{pid}/status") as f:
            status = f.read()
        check("the instance is confined (seccomp, no_new_privs)",
              "Seccomp:\t2" in status and "NoNewPrivs:\t1" in status)

        client = devtools(os.path.join(profiles, "127.0.0.1"))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, url, 20)
        for kind in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {
                "type": kind, "x": 50, "y": 40, "button": "left", "clickCount": 1}, session)
        result = wait_for(lambda: client.evaluate(session, "window.lrbResult || ''"), 15)
        check("an upload reaches the page (a copy of the chosen file)",
              result == "the user's letter", repr(result))

        client.evaluate(session, f"location.href = '{url}report'; 1")
        target = os.path.join(user, "saved", "report.txt")
        saved = wait_for(lambda: os.path.exists(target) and open(target, "rb").read(), 20)
        check("a download lands where the picker said", saved == DOWNLOAD, repr(saved))
        staged = os.path.join(profiles, "127.0.0.1", "downloads")
        check("nothing staged is left behind",
              not os.path.exists(staged) or not any(os.scandir(staged)))
    finally:
        os.killpg(coordinator.pid, 15)
        coordinator.wait(10)
        server.shutdown()
    uploads = os.path.join(profiles, "127.0.0.1", "uploads")
    check("upload copies are deleted when the instance exits", not os.path.exists(uploads))
    print(f"{failures} failure(s)")
    if not failures:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
