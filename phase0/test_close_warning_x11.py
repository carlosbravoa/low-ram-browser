"""Check the page's dialogs with real keys, and "leave this page?" on close.

  python3 test_close_warning_x11.py <path to lrb>

Opens a window on the X11 display ($DISPLAY; XWayland works) for about
half a minute. Needs network access. Keys and the close request go to lrb's
window only (lrb_harness/x11.py).

1. confirm(): the row takes focus on OK; Enter answers true.
2. prompt(): focus in its text field; Enter answers the default text.
3. Closing the window of a page with unsaved changes asks first
   (content_shell closed at once): Enter stays (focus on Stay), the window
   remains; closing again, Tab, Space (Leave) closes it.
"""

import os
import subprocess
import sys
import tempfile
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11

XK_Return, XK_Tab, XK_space = 0xff0d, 0xff09, 0x20
PAGE = "https://example.com/"


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="closewarn-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
         "--remote-debugging-port=0", "--single-process", "--no-zygote", "--no-sandbox",
         PAGE],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT,
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
        start_new_session=True)  # so runlib.stop() can kill its group
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, PAGE, 20)
        time.sleep(1)

        def window():
            found = x11.windows()
            return found[0][0] if found else None

        def keys(*keysyms):
            for keysym in keysyms:
                x11.key(window(), keysym, 0)
                time.sleep(0.4)

        def dialog(statement):
            client.call("Runtime.evaluate", {
                "expression": f"window.lrbResult = undefined; "
                              f"setTimeout(() => {{ {statement} }}, 0); 1"}, session)
            time.sleep(1)

        dialog("window.lrbResult = confirm('Delete it?');")
        keys(XK_Return)
        check("confirm(): Enter answers OK (focus on OK)",
              client.evaluate(session, "window.lrbResult") is True,
              client.evaluate(session, "window.lrbResult"))

        dialog("window.lrbResult = prompt('Your name?', 'anonymous');")
        keys(XK_Return)
        check("prompt(): Enter answers the default text",
              client.evaluate(session, "window.lrbResult") == "anonymous",
              client.evaluate(session, "window.lrbResult"))

        client.evaluate(session, """window.onbeforeunload = e => {
            e.preventDefault(); e.returnValue = ''; }; 1""")
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 5, "y": 5,
                                                     "button": "left", "clickCount": 1}, session)
        x11.close(window())
        time.sleep(1.5)
        keys(XK_Return)
        time.sleep(1)
        check("closing a page with unsaved changes asks; Enter stays",
              proc.poll() is None and window() is not None,
              f"process {'running' if proc.poll() is None else 'exited'}, "
              f"window {'open' if window() else 'gone'}")

        x11.close(window())
        time.sleep(1.5)
        keys(XK_Tab, XK_space)
        for _ in range(20):
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        check("closing again and choosing Leave closes it (last window: exits)",
              proc.poll() is not None and window() is None,
              f"process {'running' if proc.poll() is None else 'exited'}")
        client.close()
    finally:
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
