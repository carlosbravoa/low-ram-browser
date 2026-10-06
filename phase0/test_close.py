"""Check the last resort: closing a background site and restoring it.

  python3 test_close.py <dir with lrb and lrb_coordinator>

Forces critical pressure (--critical-percent=101): with two sites open, the
background one is discarded, then closed with its history saved; opening
the site again restores that history. Needs network access.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

from lrb_harness import run as runlib
from test_coordinator import devtools, instances, wait_for
from test_discard import LEAN_ARGS


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="close-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    github = os.path.join(profiles, "github.com")
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:")
    coordinator = [f"{out}/lrb_coordinator", f"--browser={out}/lrb",
                   f"--profiles-dir={profiles}", f"--socket={work}/c.sock", "--adblock-setting=off",
                   "--critical-percent=101"]
    log_path = os.path.join(work, "coordinator.log")
    log = open(log_path, "wb")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def history(profile):
        client = devtools(profile)
        session = client.attach_first_page()
        h = client.call("Page.getNavigationHistory", {}, session)
        client.close()
        return [e["url"] for e in h["entries"]], h["currentIndex"]

    proc = subprocess.Popen(
        coordinator + ["https://github.com/chromium/chromium", "--"] + LEAN_ARGS +
        ["--vmodule=lrb_content_browser_client=1,saved_windows=1,activity_tracker=1"],
        stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    try:
        wait_for(lambda: "github.com" in instances(profiles), 30)
        client = devtools(github)
        session = client.attach_first_page()
        runlib.wait_ready(client, session, "x", 30)
        # A second same-site page, so there is history to keep.
        client.evaluate(session, "location.href = 'https://github.com/chromium'; 1")
        time.sleep(1)
        runlib.wait_ready(client, session, "https://github.com/chromium", 30)
        time.sleep(2)
        before, before_index = history(github)
        # A second tab (shown on opening), then the first one in front again.
        first = [t["targetId"] for t in client.call("Target.getTargets")["targetInfos"]
                 if t["type"] == "page"][0]
        client.call("Target.createTarget", {"url": "https://github.com/explore"})
        time.sleep(3)
        client.call("Target.activateTarget", {"targetId": first})
        time.sleep(1)
        # Open another site in a new window: github.com goes to the background.
        # (Not by typing it in github.com's window: that replaces the window.)
        client.close()
        subprocess.run(coordinator + ["https://en.wikipedia.org/wiki/Web_browser"],
                       env=env, timeout=10)

        closed = wait_for(lambda: b"closing github.com" in open(log_path, "rb").read(), 60)
        check("coordinator closes the background site as a last resort", bool(closed))
        gone = wait_for(lambda: "github.com" not in instances(profiles), 20)
        check("github.com's instance exits", bool(gone), f"instances: {instances(profiles)}")
        saved = os.path.join(github, "lrb-saved-windows.json")
        check("its windows' history is saved", os.path.exists(saved))
        saved_tabs = []
        try:
            with open(saved) as f:
                saved_tabs = [len(w.get("tabs", [])) for w in json.load(f)]
        except (OSError, ValueError):
            pass
        check("...one window with its two tabs", saved_tabs == [2], f"tabs per window: {saved_tabs}")

        # Open the site again, as a user would, at the page it was showing.
        subprocess.run(coordinator + [before[before_index]], env=env, timeout=10)
        back = wait_for(lambda: "github.com" in instances(profiles), 30)
        time.sleep(8)
        restored = []
        if back:
            client = devtools(github)
            for t in client.call("Target.getTargets")["targetInfos"]:
                if t["type"] != "page":
                    continue
                s = client.call("Target.attachToTarget",
                                {"targetId": t["targetId"], "flatten": True})["sessionId"]
                h = client.call("Page.getNavigationHistory", {}, s)
                restored.append(([e["url"] for e in h["entries"]], h["currentIndex"]))
            client.close()
        check("reopening the site restores the saved history",
              any(urls[:len(before)] == before and index == before_index
                  for urls, index in restored),
              f"saved {before} at {before_index}; windows now {restored}")
        check("...in one window, only the shown tab loaded (the other loads when shown), "
              "not a duplicate for the same page",
              len(restored) == 1, f"{len(restored)} page(s) loaded")
        check("the saved state is used once", not os.path.exists(saved))
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
