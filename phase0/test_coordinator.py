"""Check lrb_coordinator with real lrb instances end to end.

  python3 test_coordinator.py <dir with lrb and lrb_coordinator>

Needs network access. Runs headless, single-process instances.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib

BROWSER_ARGS = ["--ozone-platform=headless", "--single-process", "--no-zygote",
                "--no-sandbox", "--remote-debugging-port=0",
                "--vmodule=site_window_throttle=1"]


def instances(profiles):
    """{site or '?': [pid, ...]} for lrb browser processes on `profiles`."""
    found = {}
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
        if not args or os.path.basename(args[0]) != "lrb":
            continue
        if f"--lrb-profiles-dir={profiles}" not in args or \
                any(a.startswith("--type=") for a in args):
            continue
        site = next((a.split("=", 1)[1] for a in args if a.startswith("--lrb-site=")), "?")
        found.setdefault(site, []).append(int(pid))
    return found


def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.5)
    return predicate()


def devtools(profile):
    """Browser-level CDP client for the instance using `profile`."""
    port_file = os.path.join(profile, "DevToolsActivePort")
    wait_for(lambda: os.path.exists(port_file))
    with open(port_file) as f:
        port, path = f.read().split("\n")[:2]
    return cdplib.CDP(f"ws://127.0.0.1:{port}{path}")


def pages(profile):
    port_file = os.path.join(profile, "DevToolsActivePort")
    with open(port_file) as f:
        port = f.read().split("\n")[0]
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list") as r:
        return [t["url"] for t in json.load(r) if t["type"] == "page"]


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="coord-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    sock = os.path.join(work, "c.sock")
    coordinator = [f"{out}/lrb_coordinator", f"--browser={out}/lrb",
                   f"--profiles-dir={profiles}", f"--socket={sock}", "--adblock-setting=off"]
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:")
    log = open(os.path.join(work, "coordinator.log"), "wb")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = subprocess.Popen(coordinator + ["https://en.wikipedia.org/wiki/Web_browser",
                                           "--"] + BROWSER_ARGS,
                            stdout=log, stderr=subprocess.STDOUT, env=env,
                            start_new_session=True)
    try:
        got = wait_for(lambda: "wikipedia.org" in instances(profiles) and
                       "?" not in instances(profiles), 30)
        check("command-line URL opens in its site's window, resolver exits",
              bool(got), f"instances: {instances(profiles)}")

        wiki = os.path.join(profiles, "wikipedia.org")
        client = devtools(wiki)
        client.attach_first_page()

        def wiki_page():
            """A new wikipedia.org page to leave from: typing another site
            replaces the window it is typed in (the first page keeps the
            instance alive)."""
            target = client.call("Target.createTarget",
                                 {"url": "https://en.wikipedia.org/wiki/Main_Page"})["targetId"]
            sess = client.call("Target.attachToTarget",
                               {"targetId": target, "flatten": True})["sessionId"]
            runlib.wait_ready(client, sess, "x", 20)
            return target, sess

        target, session = wiki_page()
        client.call("Page.navigate", {"url": "https://github.com/"}, session)
        got = wait_for(lambda: "github.com" in instances(profiles))
        check("typed cross-site URL opens github.com's window", bool(got),
              f"instances: {instances(profiles)}")
        check("...and the window it was typed in closes",
              wait_for(lambda: len(pages(wiki)) == 1), f"wikipedia pages: {pages(wiki)}")
        github = os.path.join(profiles, "github.com")
        wait_for(lambda: os.path.exists(os.path.join(github, "DevToolsActivePort")))
        time.sleep(2)

        target, session = wiki_page()
        client.call("Page.navigate", {"url": "https://github.com/explore"}, session)
        got = wait_for(lambda: len(pages(github)) >= 2)
        check("second github.com URL reuses its window (new same-site window)",
              len(instances(profiles).get("github.com", [])) == 1 and bool(got),
              f"github pages: {pages(github)}, instances: {instances(profiles)}")
        client.close()

        subprocess.run(coordinator + ["https://en.wikipedia.org/wiki/Main_Page"],
                       env=env, timeout=10)
        got = wait_for(lambda: len(pages(wiki)) >= 2, 30)
        check("second coordinator hands its URL to the running one",
              len(instances(profiles).get("wikipedia.org", [])) == 1 and bool(got),
              f"wikipedia pages: {pages(wiki)}")

        before = instances(profiles)
        github_pages = len(pages(github))
        raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        raw.connect(sock)
        raw.sendall(b"open github.com https://evil.example/\n"
                    b"open ../escape https://escape/\n"
                    b"open github.com file:///etc/passwd\n"
                    b"open github.com https://github.com/bad-bounds bounds=1,2,30\n"
                    b"open evil.example https://evil.example/ok bounds=0,0,800,600 back=https://github.com/ restore\n")
        time.sleep(6)
        after = instances(profiles)
        check("refuses another site's URL for github.com's window",
              len(pages(github)) == github_pages, f"github pages: {pages(github)}")
        check("refuses a site name that escapes the profiles dir",
              not os.path.exists(os.path.join(work, "escape")) and
              not any(".." in s for s in after), f"instances: {after}")
        check("a well-formed request from any instance still works",
              "evil.example" in after and set(after) - set(before) == {"evil.example"},
              f"new: {set(after) - set(before)}")
        raw.close()
    finally:
        for pids in instances(profiles).values():
            for pid in pids:
                try:
                    os.kill(pid, 15)
                except ProcessLookupError:
                    pass
        try:
            proc.wait(timeout=20)
            check("coordinator exits after the last instance", True)
        except subprocess.TimeoutExpired:
            check("coordinator exits after the last instance", False)
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
