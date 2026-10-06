"""Check lrb's permission question on a real display.

  python3 test_permission_prompt_x11.py <path to lrb>

Opens a window on the X11 display ($DISPLAY; XWayland works) for about a
minute. Needs network access. Keys go to lrb's window only (lrb_harness/
x11.py). A fake camera and microphone stand in for real ones.

1. Camera, asked after a click: the question takes focus on Block, so
   Enter blocks. Remembered: asking again is refused without a question.
2. Clipboard reading: Tab, Space (Allow) grants it. Remembered.
3. Microphone without a click: the question doesn't take focus; the page
   navigating away withdraws it: refused, not remembered. Then allowed:
   a stream starts.
4. A new instance on the same profile still knows answers 1 and 2.
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
    work = tempfile.mkdtemp(prefix="prompt-", dir=runlib.ROOT + "/.profiles")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def start():
        log = os.path.join(work, f"log-{time.monotonic_ns()}")
        proc = subprocess.Popen(
            [binary, f"--user-data-dir={work}/profile", "--lrb-site=example.com",
             "--ozone-platform=x11", "--remote-debugging-port=0", "--single-process",
             "--no-zygote", "--no-sandbox", "--use-fake-device-for-media-stream", PAGE],
            stdout=open(log, "wb"), stderr=subprocess.STDOUT,
            env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
            start_new_session=True)  # so runlib.stop() can kill its group
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, PAGE, 20)
        client.call("Emulation.setFocusEmulationEnabled", {"enabled": True}, session)
        time.sleep(1)
        return proc, client, session

    def launch(client, session, expression, gesture):
        """Starts a request in the page; its outcome lands in window.lrbResult."""
        client.call("Runtime.evaluate", {
            "expression": f"window.lrbResult = undefined; ({expression}).then("
                          "v => window.lrbResult = v, e => window.lrbResult = e.name); 1",
            "userGesture": gesture}, session)

    def result(client, session, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = client.evaluate(session, "window.lrbResult")
            if value is not None:
                return value
            time.sleep(0.3)
        return None

    def keys(*keysyms):
        window = x11.windows()[0][0]
        for keysym in keysyms:
            x11.key(window, keysym, 0)
            time.sleep(0.3)

    def state(client, session, name):
        return client.evaluate(session, f"navigator.permissions.query({{name: '{name}'}})"
                                        ".then(s => s.state)", await_promise=True)

    camera = "navigator.mediaDevices.getUserMedia({video: true}).then(s => 'granted')"
    microphone = "navigator.mediaDevices.getUserMedia({audio: true}).then(s => 'granted')"
    clipboard = "navigator.clipboard.readText().then(() => 'granted')"

    proc, client, session = start()
    try:
        launch(client, session, camera, gesture=True)
        time.sleep(1.5)
        check("camera, asked after a click, has no answer yet",
              result(client, session, 1) is None)
        keys(XK_Return)
        outcome = result(client, session)
        check("Enter on the question blocks (focus is on Block)",
              outcome == "NotAllowedError", outcome)
        launch(client, session, camera, gesture=True)
        outcome = result(client, session, 3)
        check("...and is remembered: asking again is refused without a question",
              outcome == "NotAllowedError" and state(client, session, "camera") == "denied",
              f"{outcome}, state {state(client, session, 'camera')}")

        launch(client, session, clipboard, gesture=True)
        time.sleep(1.5)
        keys(XK_Tab, XK_space)
        outcome = result(client, session)
        check("Tab, Space (Allow) grants clipboard reading", outcome == "granted", outcome)
        check("...remembered", state(client, session, "clipboard-read") == "granted",
              state(client, session, "clipboard-read"))

        launch(client, session, microphone, gesture=False)
        time.sleep(1.5)
        client.call("Page.navigate", {"url": PAGE + "?again"}, session)
        runlib.wait_ready(client, session, PAGE + "?again", 20)
        check("a question nobody answered is withdrawn when the page goes away, "
              "and not remembered", state(client, session, "microphone") == "prompt",
              state(client, session, "microphone"))

        launch(client, session, microphone, gesture=True)
        time.sleep(1.5)
        keys(XK_Tab, XK_space)
        outcome = result(client, session)
        check("allowing the microphone starts a stream (the fake device)",
              outcome == "granted", outcome)
    finally:
        client.close()
        runlib.stop(proc, None)
    time.sleep(1)

    proc, client, session = start()
    try:
        check("a new instance on the same profile remembers the answers",
              state(client, session, "camera") == "denied" and
              state(client, session, "clipboard-read") == "granted",
              f"camera {state(client, session, 'camera')}, "
              f"clipboard-read {state(client, session, 'clipboard-read')}")
    finally:
        client.close()
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
