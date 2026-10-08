"""Check the window shortcuts added for everyday use.

  python3 test_shortcuts_x11.py <path to lrb>

Opens windows on the X11 display ($DISPLAY; XWayland works) for about half
a minute. Serves its own pages (no network). Keys and clicks through X11
(lrb_harness/x11.py).

1. Ctrl+Shift+T reopens the tab closed last, with its history.
2. A middle-click on a tab in the tab row closes it.
3. Esc while a page loads stops it (the page shown stays).
4. Alt+Home goes to the site's home page.
5. F11 fills the screen without the bar, and back.
6. Ctrl+N opens a window of its own.
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

XK_t, XK_w, XK_n, XK_Escape, XK_Home, XK_F11 = 0x74, 0x77, 0x6e, 0xff1b, 0xff50, 0xffc8
ControlMask, ShiftMask, Mod1Mask = 4, 1, 8


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        name = self.path.strip("/").split("?")[0] or "home"
        if name == "slow":
            time.sleep(8)
        body = (f"<title>page {name}</title>"
                f"<a id=blank href=/c target=_blank style='position:fixed;left:10px;top:10px;"
                f"font-size:30px'>c in a new tab</a>").encode()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except BrokenPipeError:
            pass  # the slow page, stopped by Esc

    def log_message(self, *args):
        pass


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="shortcuts-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
         "--remote-debugging-port=0", base + "a"],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT,
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
        start_new_session=True)
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base + "a", 20)
        time.sleep(1)
        window = x11.windows()[0][0]

        def targets():
            return [t for t in client.call("Target.getTargets")["targetInfos"]
                    if t["type"] == "page"]

        def shown(expect, timeout=6):
            deadline = time.monotonic() + timeout
            while x11.title(window) != expect and time.monotonic() < deadline:
                time.sleep(0.3)
            return x11.title(window)

        def attach(url):
            target = next(t for t in targets() if t["url"] == url)
            return client.call("Target.attachToTarget",
                               {"targetId": target["targetId"], "flatten": True})["sessionId"]

        # 1. Ctrl+Shift+T: tab b (history a, b) closed, then back.
        client.evaluate(session, f"location.href = '{base}b'; 1")
        shown("page b")
        for kind in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": kind, "x": 40, "y": 25,
                                                     "button": "left", "clickCount": 1}, session)
        shown("page c")
        x11.key(window, ord("1"), ControlMask)
        shown("page b")
        x11.key(window, XK_w, ControlMask)
        shown("page c")
        closed = sorted(t["url"] for t in targets())
        x11.key(window, XK_t, ControlMask | ShiftMask)
        reopened = shown("page b") == "page b"
        length = client.evaluate(attach(base + "b"), "history.length") if reopened else 0
        check("Ctrl+Shift+T reopens the closed tab, with its history",
              closed == [base + "c"] and reopened and length == 2,
              f"after closing {closed}; shown {x11.title(window)}, history {length}")

        # 2. Middle-click on the first tab (the tab row is under the 36 px bar).
        before = len(targets())
        x11.click(window, 40, 36 + 14, button=2)
        time.sleep(1.5)
        check("a middle-click on a tab closes it", len(targets()) == before - 1,
              f"{before} -> {len(targets())} tabs")

        # 3. Esc while loading.
        page = attach(targets()[0]["url"])
        start_url = client.evaluate(page, "location.href")
        x11.click(window, 400, 300)  # the page has focus (Esc is the page's then)
        client.evaluate(page, f"location.href = '{base}slow'; 1")
        time.sleep(1)
        x11.key(window, XK_Escape, 0)
        time.sleep(9)  # the slow page would have arrived by now
        check("Esc stops a page loading", client.evaluate(page, "location.href") == start_url,
              client.evaluate(page, "location.href"))

        # 4. Alt+Home: the site's home page (https://127.0.0.1/: nothing there,
        # but it's where it goes).
        x11.key(window, XK_Home, Mod1Mask)
        time.sleep(2)
        check("Alt+Home goes to the site's home page",
              any(t["url"] == "https://127.0.0.1/" for t in targets()),
              str([t["url"] for t in targets()]))
        client.evaluate(page, f"location.href = '{base}a'; 1")
        shown("page a")

        # 5. F11.
        page = attach(base + "a")
        height = client.evaluate(page, "innerHeight")
        x11.key(window, XK_F11, 0)
        time.sleep(1.5)
        full = client.evaluate(page, "innerHeight")
        x11.key(window, XK_F11, 0)
        time.sleep(1.5)
        back = client.evaluate(page, "innerHeight")
        check("F11 fills the screen without the bar, and back",
              full > height + 30 and abs(back - height) < 5,
              f"page height {height} -> {full} -> {back}")

        # 6. Ctrl+N.
        x11.key(window, XK_n, ControlMask)
        time.sleep(2)
        check("Ctrl+N opens a window of its own", len(x11.windows()) == 2,
              f"{len(x11.windows())} window(s)")
        client.close()
    finally:
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
