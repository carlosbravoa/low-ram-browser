"""Check that lrb_coordinator discards background site windows under pressure.

  python3 test_discard.py <dir with lrb and lrb_coordinator>

Forces moderate pressure (--moderate-percent=101: discards, never closes),
opens two sites, and checks
that the one not in use is discarded (memory drops, window and history kept)
and reloads when brought back. Needs network access.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

from lrb_harness import procmem
from lrb_harness import run as runlib
from test_coordinator import BROWSER_ARGS, devtools, instances, pages, wait_for

# The real instance configuration (lrb-lean-*): low-end mode, software
# compositing, V8 optimised for size.
LEAN_ARGS = BROWSER_ARGS + ["--in-process-gpu", "--enable-low-end-device-mode",
                            "--disable-gpu", "--js-flags=--optimize-for-size"]


def unreclaimable_mb(pid):
    mem = procmem.smaps_rollup(pid) or {}
    return (mem.get("Pss_Anon", 0) + mem.get("Pss_Shmem", 0)) / 1024


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="discard-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:")
    log = open(os.path.join(work, "coordinator.log"), "wb")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    # Pressure from the start, but no discard until both sites are open.
    proc = subprocess.Popen(
        [f"{out}/lrb_coordinator", f"--browser={out}/lrb", f"--profiles-dir={profiles}",
         f"--socket={work}/c.sock", "--moderate-percent=101", "--critical-percent=0", "--adblock-setting=off",
         "https://github.com/chromium/chromium", "--"] + LEAN_ARGS +
        ["--vmodule=site_window_throttle=1,lrb_content_browser_client=1"],
        stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    try:
        wait_for(lambda: "github.com" in instances(profiles), 30)
        github = os.path.join(profiles, "github.com")
        client = devtools(github)
        session = client.attach_first_page()
        runlib.wait_ready(client, session, "x", 30)
        time.sleep(3)
        gh_pid = instances(profiles)["github.com"][0]
        before = unreclaimable_mb(gh_pid)

        # Open a second site in a new window: github.com stops being the most
        # recently used. (Typed in github.com's window, it would replace it.)
        subprocess.run([f"{out}/lrb_coordinator", f"--browser={out}/lrb",
                        f"--profiles-dir={profiles}", f"--socket={work}/c.sock",
                        "https://en.wikipedia.org/wiki/Web_browser"], env=env, timeout=10)
        wait_for(lambda: "wikipedia.org" in instances(profiles), 30)
        # Detach DevTools: an attached session keeps the page's objects alive
        # (console messages, inspected nodes), which a real user's window
        # doesn't have. Reattached below to bring the window back.
        client.close()

        discarded = wait_for(lambda: b"discarding github.com" in open(
            os.path.join(work, "coordinator.log"), "rb").read(), 40)
        check("coordinator discards the window not in use (github.com)", bool(discarded))
        timeline = []
        for _ in range(8):
            time.sleep(5)
            timeline.append(round(unreclaimable_mb(gh_pid)))
        print(f"      github.com unreclaimable before {before:.0f} MB, every 5 s after "
              f"discard: {timeline}")
        after = unreclaimable_mb(gh_pid)
        check("github.com's memory drops", after < before * 0.7,
              f"{before:.0f} MB -> {after:.0f} MB unreclaimable")
        check("its window and history survive", pages(github) != [] and
              gh_pid in instances(profiles).get("github.com", []),
              f"pages: {pages(github)}")
        check("the window in use (wikipedia.org) is not discarded",
              b"discarding wikipedia.org" not in open(
                  os.path.join(work, "coordinator.log"), "rb").read())

        # Come back to github.com, as a user: bring it forward and click.
        client = devtools(github)
        session = client.attach_first_page()
        client.call("Page.bringToFront", {}, session)
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 5, "y": 5,
                                                     "button": "left", "clickCount": 1},
                        session)
        reloaded = wait_for(lambda: unreclaimable_mb(gh_pid) > after + 20, 30)
        title = client.evaluate(session, "document.title") if reloaded else None
        check("a discarded page reloads when its window is used again",
              bool(reloaded) and bool(title),
              f"{unreclaimable_mb(gh_pid):.0f} MB, title: {title!r}")
        history = client.call("Page.getNavigationHistory", {}, session)
        urls = [e["url"] for e in history["entries"]]
        check("history is as before the discard (no blank entry left)",
              "about:blank" not in urls[1:] and urls[history["currentIndex"]].startswith(
                  "https://github.com/"), f"history: {urls} at {history['currentIndex']}")
        client.close()
    finally:
        for pids in instances(profiles).values():
            for pid in pids:
                try:
                    os.kill(pid, 15)
                except ProcessLookupError:
                    pass
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, 9)
        log.close()
        if failures:
            print(f"kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
