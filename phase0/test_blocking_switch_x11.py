"""Check turning content blocking off (and on) for a site.

  python3 test_blocking_switch_x11.py <path to lrb>

Opens a window on the X11 display ($DISPLAY; XWayland works) for about a
minute. Needs network access (a tag-manager script the privacy lists
block) and the full engine (dist/adblock/full.adb, or $LRB_ADBLOCK_DIR).
The menu is opened with Alt+F through X11 (lrb_harness/x11.py).

1. Blocking on: the tracker script doesn't run.
2. Menu, "Turn off content blocking on <site>": the page reloads and the
   script runs.
3. A new instance on the same profile: still off.
4. Turning it on again: blocked again.
"""

import http.server
import os
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11

XK_Down, XK_Return, XK_f, Mod1Mask = 0xff54, 0xff0d, 0x66, 8
PAGE = (b"<title>tracked</title><script async "
        b"src='https://www.googletagmanager.com/gtag/js?id=G-LRBTEST'></script>")
ENGINE = os.path.join(os.environ.get("LRB_ADBLOCK_DIR", os.path.join(runlib.ROOT, "dist", "adblock")),
                      "full.adb")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(PAGE)))
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, *args):
        pass


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    work = tempfile.mkdtemp(prefix="blockswitch-", dir=runlib.ROOT + "/.profiles")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def start():
        log = os.path.join(work, f"log-{time.monotonic_ns()}")
        proc = subprocess.Popen(
            [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
             "--remote-debugging-port=0", f"--lrb-adblock-file={os.path.abspath(ENGINE)}",
             base],
            stdout=open(log, "wb"), stderr=subprocess.STDOUT,
            env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
            start_new_session=True)  # so runlib.stop() can kill its group
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)
        time.sleep(4)
        return proc, client, session

    def tracker_ran(client, session):
        return client.evaluate(session, "typeof window.google_tag_manager !== 'undefined'")

    def toggle(client, session):
        window = x11.windows()[0][0]
        x11.key(window, XK_f, Mod1Mask)  # the menu
        time.sleep(0.8)
        # New tab, New window, Close tab, Print to PDF, Dark pages, Turn off/on
        # blocking
        for _ in range(6):
            x11.key(window, XK_Down, 0)
            time.sleep(0.3)
        x11.key(window, XK_Return, 0)
        time.sleep(1)
        runlib.wait_ready(client, session, base, 20)
        time.sleep(4)

    proc, client, session = start()
    try:
        check("blocking on: the tracker script doesn't run", not tracker_ran(client, session))
        toggle(client, session)
        check("turned off for the site: the page reloads and the script runs",
              tracker_ran(client, session))
    finally:
        client.close()
        runlib.stop(proc, None)
    time.sleep(1)

    proc, client, session = start()
    try:
        check("still off in a new instance on the same profile", tracker_ran(client, session))
        toggle(client, session)
        check("turned on again: blocked again", not tracker_ran(client, session))
    finally:
        client.close()
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
