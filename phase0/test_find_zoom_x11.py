"""Check find in page (Ctrl+F) and zoom (Ctrl +/-/0, kept per site).

  python3 test_find_zoom_x11.py <path to lrb>

Opens a window on the X11 display ($DISPLAY; XWayland works) for about
half a minute. Serves its own page (no network). Keys through X11
(lrb_harness/x11.py).

1. Ctrl+F, typing "apple": the first match is the active one; Enter moves
   to the second; Escape closes find and leaves that match selected.
2. Ctrl+= zooms in a step (110%), Ctrl+0 resets (devicePixelRatio).
3. Two steps (125%), then a new instance on the same profile: still 125%.
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

XK_Return, XK_Escape, XK_equal, XK_0, ControlMask = 0xff0d, 0xff1b, 0x3d, 0x30, 4
PAGE = (b'<p id="one">an apple</p><p id="two">another apple</p>'
        b'<p id="three">a third apple</p><p>a banana</p>')


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
    work = tempfile.mkdtemp(prefix="find-", dir=runlib.ROOT + "/.profiles")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def start():
        log = os.path.join(work, f"log-{time.monotonic_ns()}")
        proc = subprocess.Popen(
            [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
             "--remote-debugging-port=0", "--single-process", "--no-zygote", "--no-sandbox",
             base],
            stdout=open(log, "wb"), stderr=subprocess.STDOUT,
            env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
            start_new_session=True)  # so runlib.stop() can kill its group
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)
        time.sleep(1.5)
        return proc, client, session

    def keys(*pairs):
        window = x11.windows()[0][0]
        for keysym, state in pairs:
            x11.key(window, keysym, state)
            time.sleep(0.3)

    def ratio(client, session):
        return round(client.evaluate(session, "devicePixelRatio"), 2)

    proc, client, session = start()
    try:
        keys((ord("f"), ControlMask), *[(ord(c), 0) for c in "apple"])
        time.sleep(1)
        keys((XK_Return, 0))
        time.sleep(0.5)
        keys((XK_Escape, 0))
        time.sleep(0.5)
        selected = client.evaluate(session, "getSelection().toString()")
        where = client.evaluate(session,
                                "getSelection().anchorNode?.parentElement?.id || ''")
        check("find: typing finds, Enter goes to the next match, Escape keeps it selected",
              selected == "apple" and where == "two", f"selected '{selected}' in '{where}'")

        start_ratio = ratio(client, session)
        keys((XK_equal, ControlMask))
        time.sleep(1)
        zoomed = ratio(client, session)
        keys((XK_0, ControlMask))
        time.sleep(1)
        check("zoom: Ctrl+= one step (110%), Ctrl+0 resets",
              zoomed == round(start_ratio * 1.1, 2) and ratio(client, session) == start_ratio,
              f"{start_ratio} -> {zoomed} -> {ratio(client, session)}")
        keys((XK_equal, ControlMask), (XK_equal, ControlMask))
        time.sleep(1.5)
    finally:
        client.close()
        runlib.stop(proc, None)
    time.sleep(1)

    proc, client, session = start()
    try:
        check("zoom is kept for the site: 125% after restarting",
              ratio(client, session) == round(start_ratio * 1.25, 2),
              f"{ratio(client, session)}")
    finally:
        client.close()
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
