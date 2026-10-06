"""Check file upload without a picker to ask with (content_shell cancelled
every file chooser at once, silently).

  python3 test_uploads.py <path to lrb>

The harness has no session bus, so there is no file picker: choosing a
file ends as cancelled (the page gets its "cancel" event, no files), and
the window says why. With the desktop portal: test_portal_picker.py.
"""

import os
import sys
import tempfile
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib

PAGE = ("data:text/html,<input id=f type=file multiple>"
        "<script>f.addEventListener('cancel', () => window.lrbResult = 'cancel');"
        "f.addEventListener('change', () => window.lrbResult = 'files ' + f.files.length);"
        "</script>")


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="uploads-", dir=runlib.ROOT + "/.profiles")
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
        session = client.attach_first_page()
        client.call("Page.navigate", {"url": PAGE}, session)
        time.sleep(2)
        client.call("Runtime.evaluate", {"expression": "f.click(); 1", "userGesture": True},
                    session)
        result = None
        for _ in range(20):
            result = client.evaluate(session, "window.lrbResult")
            if result:
                break
            time.sleep(0.3)
        check("with no picker, choosing files ends as cancelled (no hang)",
              result == "cancel", result)
        check("...and no files are chosen", client.evaluate(session, "f.files.length") == 0)
    finally:
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
