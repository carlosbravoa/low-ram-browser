"""Check the page's dialogs: alert, confirm, prompt, "leave this page?".

  python3 test_dialogs.py <path to lrb>

content_shell shows none on Linux (confirm() answered "no", silently). lrb
shows them in the window's question row; DevTools answers them here
(Page.handleJavaScriptDialog goes through lrb's dialog manager, as a
click on the row would). Keys and the close button: test_close_warning_x11.py.
Needs network access.
"""

import os
import sys
import tempfile
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib

PAGE = "https://example.com/"


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="dialogs-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    proc = runlib.launch([binary, f"--user-data-dir={work}/p", "--ozone-platform=headless",
                          "--single-process", "--no-zygote", "--no-sandbox",
                          "--remote-debugging-port=0", "about:blank"], log, None, False)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        client.keep_events = True
        session = client.attach_first_page()
        client.call("Page.enable", {}, session)
        client.call("Page.navigate", {"url": PAGE}, session)
        runlib.wait_ready(client, session, PAGE, 20)

        def open_dialog(statement):
            """Runs `statement` (which opens a dialog) after this call returns:
            a dialog blocks the page until answered."""
            client.events.clear()
            client.call("Runtime.evaluate", {
                "expression": f"window.lrbResult = undefined; "
                              f"setTimeout(() => {{ {statement} }}, 0); 1"}, session)
            return client.wait_event("Page.javascriptDialogOpening")["params"]

        def answer(accept, text=None):
            params = {"accept": accept}
            if text is not None:
                params["promptText"] = text
            client.call("Page.handleJavaScriptDialog", params, session)
            time.sleep(0.5)
            return client.evaluate(session, "window.lrbResult")

        opened = open_dialog("alert('Hello from the page'); window.lrbResult = 'done';")
        check("alert() is shown", opened["type"] == "alert" and
              opened["message"] == "Hello from the page", opened)
        check("...and returns once answered", answer(True) == "done")

        open_dialog("window.lrbResult = confirm('Delete it?');")
        check("confirm(): OK answers true", answer(True) is True)
        open_dialog("window.lrbResult = confirm('Delete it?');")
        check("confirm(): Cancel answers false", answer(False) is False)

        opened = open_dialog("window.lrbResult = prompt('Your name?', 'anonymous');")
        check("prompt() is shown with its default text",
              opened["type"] == "prompt" and opened.get("defaultPrompt") == "anonymous",
              opened)
        check("...answers what was typed", answer(True, "lrb") == "lrb")
        # (Accepted untouched, the default text: test_close_warning_x11.py.
        # DevTools answers with "" when given no text, before lrb's row.)
        open_dialog("window.lrbResult = prompt('Your name?', 'anonymous');")
        check("...or null when cancelled", answer(False) is None)

        # "Leave this page?": only for a page the user interacted with.
        client.evaluate(session, """window.onbeforeunload = e => {
            e.preventDefault(); e.returnValue = ''; }; 1""")
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 5, "y": 5,
                                                     "button": "left", "clickCount": 1}, session)
        opened = open_dialog(f"location.href = '{PAGE}?next';")
        check("leaving a page with unsaved changes asks first",
              opened["type"] == "beforeunload", opened)
        answer(False)
        time.sleep(1)
        check("...Stay keeps the page", client.evaluate(session, "location.href") == PAGE,
              client.evaluate(session, "location.href"))
        open_dialog(f"location.href = '{PAGE}?next';")
        client.call("Page.handleJavaScriptDialog", {"accept": True}, session)
        runlib.wait_ready(client, session, PAGE + "?next", 20)
        check("...Leave leaves", client.evaluate(session, "location.href") == PAGE + "?next",
              client.evaluate(session, "location.href"))
    finally:
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
