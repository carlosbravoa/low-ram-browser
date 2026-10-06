"""Check that background tabs sleep under memory pressure, before anything
closes (decided 2026-10-05: tabs stay live; pressure sleeps them).

  python3 test_tab_sleep.py <dir with lrb and lrb_coordinator>

The coordinator runs under constant (simulated) moderate pressure with one
site, two tabs: with no other site to discard, it has the site's
background tab sleep. Serves its own pages (no network); headless.

1. The background tab sleeps (about:blank in place of its page); the shown
   tab is untouched; nothing closes.
2. Bringing the tab to the front brings its page back.
"""

import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness.session import LEAN_ARGS
from test_coordinator import devtools, instances, wait_for


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"<title>{self.path}</title>{self.path}".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    out = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    work = tempfile.mkdtemp(prefix="tabsleep-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    site = os.path.join(profiles, "127.0.0.1")
    log_path = os.path.join(work, "coordinator.log")
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    # No pressure at first (so nothing sleeps while the tabs open), then
    # constant moderate pressure: a second coordinator run can't change the
    # first's settings, so the pressure comes from the start with tabs
    # opened quickly; the coordinator acts at most every 10 s.
    proc = subprocess.Popen(
        [f"{out}/lrb_coordinator", f"--browser={out}/lrb", f"--profiles-dir={profiles}",
         f"--socket={work}/c.sock", "--adblock-setting=off", "--moderate-percent=101",
         "--critical-percent=0", "--verbose", base + "shown", "--"] + LEAN_ARGS,
        stdout=open(log_path, "wb"), stderr=subprocess.STDOUT, env=env,
        start_new_session=True)
    try:
        wait_for(lambda: "127.0.0.1" in instances(profiles), 30)
        client = devtools(site)
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base + "shown", 20)
        first = [t["targetId"] for t in client.call("Target.getTargets")["targetInfos"]
                 if t["type"] == "page"][0]
        background = client.call("Target.createTarget", {"url": base + "background"})["targetId"]
        time.sleep(2)
        client.call("Target.activateTarget", {"targetId": first})

        def url_of(target):
            for t in client.call("Target.getTargets")["targetInfos"]:
                if t["targetId"] == target:
                    return t["url"]
            return None

        # The coordinator acts every 10 s from the start: whichever tab was in
        # the background then sleeps, and a shown tab wakes. In the end: the
        # background tab asleep, the shown one awake.
        slept = wait_for(lambda: url_of(background) == "about:blank" and
                         url_of(first) == base + "shown", 30)
        check("the background tab sleeps under pressure; the shown one is awake",
              bool(slept), f"background {url_of(background)}, shown {url_of(first)}")
        log = open(log_path, errors="replace").read()
        check("...and nothing closes", "closing" not in log and
              "sleeping 1 background tab(s)" in log,
              [l for l in log.splitlines() if "pressure" in l][-3:])

        client.call("Target.activateTarget", {"targetId": background})
        back = wait_for(lambda: url_of(background) == base + "background", 15)
        check("bringing it to the front brings its page back", bool(back),
              f"shows {url_of(background)}")
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
        server.shutdown()
        if not failures:
            shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
