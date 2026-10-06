"""Check leaving a site and coming back with Back, on a real display.

  python3 test_back_x11.py <path to lrb>

Opens windows on the X11 display ($DISPLAY; XWayland works) for about half
a minute. Needs network access. One lrb started by hand, as a user would:

1. Wikipedia, two pages of history.
2. Typing another site's address replaces the window: the new site's
   window opens and Wikipedia's process ends, its window saved.
3. Alt+Left on the new site's first page brings Wikipedia back, restored.
4. Alt+Left again goes back within Wikipedia's restored history.
5. The close button closes it and nothing is left running.
"""

import os
import subprocess
import sys
import tempfile
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="back-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    log = os.path.join(work, "first.log")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def instances():
        found = []
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    args = [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
            except OSError:
                continue
            if len(args) == 1:
                args = args[0].split()
            if args and os.path.basename(args[0]) == "lrb" and \
                    f"--lrb-profiles-dir={profiles}" in args and \
                    not any(a.startswith("--type=") for a in args):
                found.append(next((a.split("=", 1)[1] for a in args
                                   if a.startswith("--lrb-site=")), "first"))
        return sorted(found)

    def titles():
        return [t for _, t in x11.windows()]

    def wait_for(predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return True
            time.sleep(0.5)
        return predicate()

    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/first", f"--lrb-profiles-dir={profiles}",
         "--ozone-platform=x11", "--remote-debugging-port=0", "--single-process",
         "--no-zygote", "--no-sandbox", "https://en.wikipedia.org/wiki/Web_browser"],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT,
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
        start_new_session=True)  # so runlib.stop() can kill its group
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, "x", 20)
        main_page = "https://en.wikipedia.org/wiki/Main_Page"
        client.call("Page.navigate", {"url": main_page}, session)
        runlib.wait_ready(client, session, main_page, 20)
        time.sleep(1)

        client.call("Page.navigate", {"url": "https://www.iana.org/"}, session)
        client.close()
        check("typed address replaces the window",
              wait_for(lambda: instances() == ["iana.org"] and proc.poll() is not None)
              and wait_for(lambda: titles() == ["Internet Assigned Numbers Authority"]),
              f"instances {instances()}, windows {titles()}")

        x11.key(x11.windows()[0][0], x11.XK_Left, x11.Mod1Mask)
        check("Back brings Wikipedia back in its place",
              wait_for(lambda: instances() == ["wikipedia.org"]) and
              wait_for(lambda: titles() == ["Wikipedia, the free encyclopedia"]),
              f"instances {instances()}, windows {titles()}")

        x11.key(x11.windows()[0][0], x11.XK_Left, x11.Mod1Mask)
        check("...with its history",
              wait_for(lambda: titles() == ["Web browser - Wikipedia"]),
              f"windows {titles()}")

        for window, _ in x11.windows():
            x11.close(window)
        check("the close button leaves nothing running",
              wait_for(lambda: not instances() and not titles()),
              f"instances {instances()}, windows {titles()}")
    finally:
        runlib.stop(proc, None)
        subprocess.run(["pkill", "-9", "-f", "--", f"--lrb-profiles-dir={profiles}"])
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
