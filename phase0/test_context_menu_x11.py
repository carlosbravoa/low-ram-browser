"""Check the right-click menu (content_shell's had only "Inspect Element").

  python3 test_context_menu_x11.py <path to lrb>

Opens a window on the X11 display ($DISPLAY; XWayland works) for about
half a minute. Serves its own page (no network). Right clicks go in
through DevTools, the keys that choose an item through X11
(lrb_harness/x11.py); what was copied is checked by pasting it (Ctrl+V)
into a text field, after a real click into the page (lrb_harness/x11.py:
keys reach the page only once it has keyboard focus).

1. On a link: "Open link in new window" opens it in a new window.
2. On a link: "Copy link address" copies it.
3. On selected text: "Copy" copies it.
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

XK_Down, XK_Return, XK_v, ControlMask = 0xff54, 0xff0d, 0x76, 4
PAGE = (b'<a id="l" href="/other" style="font-size:40px;position:fixed;left:20px;top:20px">'
        b'a link</a><p id="p" style="position:fixed;left:20px;top:90px;font-size:30px;margin:0">'
        b'some selected words</p><textarea id="t" style="position:fixed;left:20px;'
        b'top:160px"></textarea>')


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
    work = tempfile.mkdtemp(prefix="menu-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
         "--remote-debugging-port=0", "--single-process", "--no-zygote", "--no-sandbox", base],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT,
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
        start_new_session=True)  # so runlib.stop() can kill its group
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)
        time.sleep(1)
        window = x11.windows()[0][0]
        # A real click into the page: keyboard focus on the page, as after a
        # user's click (DevTools' clicks don't move it).
        x11.click(window, 300, 400)

        def menu_item(x, y, downs):
            """Right-clicks at (x, y), then picks the `downs`-th item."""
            x11.activate(window)
            time.sleep(0.4)
            for event in ("mousePressed", "mouseReleased"):
                client.call("Input.dispatchMouseEvent", {"type": event, "x": x, "y": y,
                                                         "button": "right", "clickCount": 1},
                            session)
            time.sleep(1.2)
            for _ in range(downs):
                x11.key(window, XK_Down, 0)
                time.sleep(0.3)
            x11.key(window, XK_Return, 0)
            time.sleep(1)

        def pasted():
            client.evaluate(session, "t.value = ''; t.focus(); 1")
            x11.key(window, XK_v, ControlMask)
            time.sleep(0.8)
            return client.evaluate(session, "t.value")

        def pages():
            return [t["url"] for t in client.call("Target.getTargets")["targetInfos"]
                    if t["type"] == "page"]

        menu_item(40, 40, 1)
        check("link: Open link in new window", base + "other" in pages(), pages())
        # Same site: it opened as a tab, now in front. Back to the first.
        x11.key(window, ord("1"), 4)  # Ctrl+1
        time.sleep(1)

        menu_item(40, 40, 2)
        check("link: Copy link address", pasted() == base + "other",
              client.evaluate(session, "t.value"))

        # As selecting with the mouse would: nothing focused, the text selected.
        client.evaluate(session, """{ document.activeElement.blur();
            const r = document.createRange();
            r.selectNodeContents(document.getElementById('p'));
            getSelection().removeAllRanges(); getSelection().addRange(r); } 1""")
        menu_item(60, 105, 1)
        check("selection: Copy", pasted() == "some selected words",
              client.evaluate(session, "t.value"))
        client.close()
    finally:
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
