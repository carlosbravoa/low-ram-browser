"""Check that no site gets a permission the user hasn't given.

  python3 test_permissions.py <path to lrb or content_shell>

Decided 2026-10-04: deny by default; content_shell granted location,
clipboard reading, notifications, MIDI, ... silently. Asks the way a page
would, on a real https page (a secure context), and checks the answers:
nothing granted; what lrb asks the user about (camera, microphone,
clipboard reading) reports "prompt", everything else is refused outright.
Speech recognition (it listens to the microphone) is refused too.
Answering the question: test_permission_prompt_x11.py. Writing to the
clipboard after a click ("copy" buttons) still works. Needs network access.
"""

import os
import sys
import tempfile
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib

# navigator.permissions.query names: none may be granted.
QUERIED = ["geolocation", "notifications", "camera", "microphone", "clipboard-read",
           "midi", "persistent-storage", "screen-wake-lock", "background-sync",
           "accelerometer", "idle-detection", "payment-handler"]

# Asked about in lrb (a question in the window), not granted until answered.
ASKED = ["camera", "microphone", "clipboard-read"]

# Real requests for what lrb never asks about, each resolving to a short
# outcome string: refused without a question.
REQUESTS = {
    "location": """new Promise(r => navigator.geolocation.getCurrentPosition(
        () => r('granted'), e => r(e.code === 1 ? 'denied' : 'error ' + e.code),
        {timeout: 5000}))""",
    "notifications": "Notification.requestPermission()",
    # Listens to the microphone (content_shell allowed it without asking).
    "speech recognition": """new Promise(r => {
        const s = new webkitSpeechRecognition();
        s.onerror = e => r(e.error === 'not-allowed' ? 'denied' : 'error ' + e.error);
        s.onaudiostart = () => r('granted');
        setTimeout(() => r('no answer'), 5000);
        s.start();
    })""",
}


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="perm-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    proc = runlib.launch([binary, f"--user-data-dir={work}/p", "--ozone-platform=headless",
                          "--single-process", "--no-zygote", "--no-sandbox",
                          "--remote-debugging-port=0",
                          # A fake camera and microphone, so a request gets as
                          # far as the permission decision (headless has none).
                          "--use-fake-device-for-media-stream", "about:blank"],
                         log, None, False)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        client.call("Page.navigate", {"url": "https://example.com/"}, session)
        runlib.wait_ready(client, session, "https://example.com/", 20)
        # The clipboard API wants a focused document.
        client.call("Emulation.setFocusEmulationEnabled", {"enabled": True}, session)

        def run(expression, gesture=True):
            reply = client.call("Runtime.evaluate", {
                "expression": expression, "awaitPromise": True, "returnByValue": True,
                "userGesture": gesture}, session)
            if "exceptionDetails" in reply:
                return "exception: " + reply["exceptionDetails"].get("text", "")
            return reply["result"].get("value")

        states = run("Promise.all(%r.map(n => navigator.permissions.query({name: n})"
                     ".then(s => s.state, e => 'unsupported')))" % QUERIED)
        granted = [n for n, s in zip(QUERIED, states) if s == "granted"]
        check("no permission is granted (permissions.query)", not granted,
              f"granted: {granted}; " + ", ".join(f"{n} {s}" for n, s in zip(QUERIED, states)))
        others = {n: s for n, s in zip(QUERIED, states) if n not in ASKED}
        check("what lrb doesn't ask about is refused outright",
              all(s in ("denied", "unsupported") for s in others.values()), others)
        asked = {n: s for n, s in zip(QUERIED, states) if n in ASKED}
        check("what lrb asks about waits for the user ('prompt')",
              all(s == "prompt" for s in asked.values()), asked)
        for name, expression in REQUESTS.items():
            outcome = run(expression)
            check(f"{name}: requested, refused without a question",
                  outcome == "denied", outcome)
        outcome = run("navigator.clipboard.writeText('lrb').then(() => 'written', e => e.name)")
        check("writing to the clipboard after a click still works (copy buttons)",
              outcome == "written", outcome)
    finally:
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
