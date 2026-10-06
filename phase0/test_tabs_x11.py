"""Check tabs (decided 2026-10-05): new pages of a site open as tabs.

  python3 test_tabs_x11.py <path to lrb>

Opens windows on the X11 display ($DISPLAY; XWayland works) for about half
a minute. Serves its own pages (no network). Keys through X11
(lrb_harness/x11.py); the shown tab is read from the window's title.

1. A target=_blank link opens a tab in the same window, in front.
2. Ctrl+1 / Ctrl+Tab switch tabs; tabs switched away from stay live.
3. A link opened in the background (middle-click) is a tab that isn't
   loaded (no page) until shown: Ctrl+3 loads it.
4. A popup with an opener (window.open: sign-in, payment) gets its own
   window.
5. Ctrl+W closes the shown tab; the window stays with the others.
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

XK_Tab, XK_w, ControlMask = 0xff09, 0x77, 4


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        name = self.path.strip("/").split("?")[0] or "a"
        body = (f"<title>page {name}</title>"
                f"<a id=blank href=/b target=_blank style='position:fixed;left:10px;top:10px;"
                f"font-size:30px'>b in a new tab</a>"
                f"<a id=bg href=/c style='position:fixed;left:10px;top:70px;"
                f"font-size:30px'>c in the background</a>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    work = tempfile.mkdtemp(prefix="tabs-", dir=runlib.ROOT + "/.profiles")
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
        start_new_session=True)  # so runlib.stop() can kill its group
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base + "a", 20)
        time.sleep(1)
        window = x11.windows()[0][0]

        def pages():
            return sorted(t["url"].rsplit("/", 1)[1] for t in
                          client.call("Target.getTargets")["targetInfos"]
                          if t["type"] == "page")

        def shown(expect=None):
            """The shown tab's title (waiting, up to 6 s, for `expect`)."""
            deadline = time.monotonic() + 6
            time.sleep(0.8)
            while expect and x11.title(window) != expect and time.monotonic() < deadline:
                time.sleep(0.3)
            return x11.title(window)

        def click(x, y, button="left"):
            for event in ("mousePressed", "mouseReleased"):
                client.call("Input.dispatchMouseEvent", {"type": event, "x": x, "y": y,
                                                         "button": button, "clickCount": 1},
                            session)

        click(40, 25)
        check("a target=_blank link opens a tab in the same window, in front",
              shown("page b") == "page b" and pages() == ["a", "b"] and len(x11.windows()) == 1,
              f"pages {pages()}, windows {len(x11.windows())}, shown {x11.title(window)}")

        x11.key(window, ord("1"), ControlMask)
        first = shown()
        x11.key(window, XK_Tab, ControlMask)
        check("Ctrl+1 and Ctrl+Tab switch tabs; the other stays live",
              first == "page a" and shown() == "page b" and pages() == ["a", "b"],
              f"{first}, then {x11.title(window)}; pages {pages()}")

        x11.key(window, ord("1"), ControlMask)
        shown()
        click(40, 85, button="middle")
        time.sleep(1)
        check("a link opened in the background adds a tab without loading it",
              pages() == ["a", "b"] and shown() == "page a", f"pages {pages()}")
        x11.key(window, ord("2"), ControlMask)  # the background tab, after a
        check("...which loads when shown", shown("page c") == "page c" and "c" in pages(),
              f"pages {pages()}, shown {x11.title(window)}")

        x11.key(window, ord("1"), ControlMask)
        shown()
        client.call("Runtime.evaluate", {
            "expression": f"window.lrbPopup = window.open('{base}popup', 'p', "
                          f"'width=300,height=300'); 1", "userGesture": True}, session)
        time.sleep(2)
        check("a popup with an opener gets its own window", len(x11.windows()) == 2,
              f"{len(x11.windows())} window(s)")
        client.evaluate(session, "window.lrbPopup.close(); 1")
        time.sleep(1)

        before = pages()
        x11.key(window, ord("2"), ControlMask)
        shown()
        x11.key(window, XK_w, ControlMask)
        time.sleep(1.5)
        check("Ctrl+W closes the shown tab; the window stays with the others",
              len(pages()) == len(before) - 1 and len(x11.windows()) == 1 and
              x11.title(window).startswith("page"),
              f"pages {before} -> {pages()}, shown {x11.title(window)}")
        client.close()
    finally:
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
