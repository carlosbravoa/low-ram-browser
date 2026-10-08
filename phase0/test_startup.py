"""Check what lrb opens when started without an address, and the session.

  python3 test_startup.py <dir with lrb and lrb_coordinator>

Through the coordinator, headless, local pages only; settings, data and
profiles in a temporary directory (never the user's).

1. A site's windows are saved as they change: after a kill (a crash, a
   logout), starting lrb without an address opens the last site with its
   page and history as they were (--lrb-resume).
2. Closing the site's windows yourself forgets them: the next start opens
   the last site at the address that opened it, with no old history.
3. Settings "startup": "blank" opens an empty window, "page" the page given.
4. Settings "content_blocking": the coordinator fetches the lists of the
   level chosen ("lean" here), and none for "off".
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import http.server
import time

from lrb_harness import run as runlib
from test_coordinator import devtools, instances, wait_for
from test_discard import LEAN_ARGS

SITE = "127.0.0.1"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        name = self.path.strip("/") or "a"
        body = f"<title>page {name}</title>page {name}".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="startup-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    profile = os.path.join(profiles, SITE)
    config = os.path.join(work, "config")
    os.makedirs(os.path.join(config, "lrb"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:",
               XDG_CONFIG_HOME=config, XDG_DATA_HOME=os.path.join(work, "data"))
    failures = 0
    coordinator = None
    log_path = os.path.join(work, "coordinator.log")

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def settings(**values):
        values.setdefault("gpu", False)
        values.setdefault("content_blocking", "off")  # no list downloads
        with open(os.path.join(config, "lrb", "settings.json"), "w") as f:
            json.dump(values, f)

    def start(url=None):
        nonlocal coordinator
        port_file = os.path.join(profile, "DevToolsActivePort")
        if os.path.exists(port_file):
            os.remove(port_file)  # the killed instance's
        args = [f"{out}/lrb_coordinator", f"--browser={out}/lrb", "--verbose",
                f"--profiles-dir={profiles}", f"--socket={work}/c.sock"]
        coordinator = subprocess.Popen(
            args + ([url] if url else []) + ["--"] + LEAN_ARGS,
            stdout=open(log_path, "a"), stderr=subprocess.STDOUT, env=env,
            start_new_session=True)

    def kill():
        """Everything, at once, as a crash or a power cut would."""
        for pids in instances(profiles).values():
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        if coordinator and coordinator.poll() is None:
            os.killpg(coordinator.pid, signal.SIGKILL)
            coordinator.wait(10)
        wait_for(lambda: not instances(profiles), 10)

    def page():
        """The site window's address and history length."""
        if not wait_for(lambda: SITE in instances(profiles), 30):
            return None, None, (None, 0)
        client = devtools(profile)
        session = client.attach_first_page()
        wait_for(lambda: client.evaluate(session, "document.readyState") == "complete", 15)
        state = (client.evaluate(session, "location.href"),
                 client.evaluate(session, "history.length"))
        return client, session, state

    def cmdline(pid):
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return f.read().replace(b"\0", b" ").decode()

    saved = os.path.join(profile, "lrb-saved-windows.json")
    try:
        # 1. Saved as it changes; back after a kill.
        settings()
        start(base + "a")
        client, session, _ = page()
        client.evaluate(session, f"location.href = '{base}b'; 1")
        wait_for(lambda: client.evaluate(session, "location.href") == base + "b", 10)
        has_b = wait_for(lambda: os.path.exists(saved) and
                         base + "b" in open(saved).read(), 10)
        check("the session is saved as pages change", bool(has_b))
        client.close()
        kill()
        start()
        resumed = wait_for(lambda: instances(profiles).get(SITE), 30)
        check("a start without an address opens the last site, resuming",
              bool(resumed) and "--lrb-resume" in cmdline(resumed[0]),
              "" if resumed and "--lrb-resume" in cmdline(resumed[0])
              else cmdline(resumed[0]) if resumed else "no instance")
        client, session, (url, length) = page()
        check("...with its page and history as they were",
              url == base + "b" and length == 2, f"{url}, history {length}")

        # 2. Closed by the user: forgotten.
        target = next(t for t in client.call("Target.getTargets")["targetInfos"]
                      if t["type"] == "page")
        client.call("Target.closeTarget", {"targetId": target["targetId"]})
        gone = wait_for(lambda: not instances(profiles).get(SITE), 15)
        check("closing the site's windows ends its instance", bool(gone))
        check("...and forgets its session", not os.path.exists(saved))
        coordinator.wait(15)
        start()
        _, _, (url, length) = page()
        check("the next start opens the last site at the address that opened it",
              url == base + "a" and length == 1, f"{url}, history {length}")
        kill()

        # 3. Blank, or a page.
        settings(startup="blank")
        start()
        time.sleep(6)
        check("startup blank: an empty window, no site opened",
              SITE not in instances(profiles), f"instances {instances(profiles)}")
        kill()
        settings(startup="page", startup_page=base + "b")
        start()
        _, _, (url, _) = page()
        check("startup page: the page given", url == base + "b", url)
        kill()

        # 4. Content blocking level from the settings.
        open(log_path, "w").close()
        settings(startup="blank", content_blocking="lean")
        start()
        lean = wait_for(lambda: "updating filter lists (lean)" in open(log_path).read(), 10)
        check("content_blocking lean: the lean lists are fetched", bool(lean))
        kill()
        open(log_path, "w").close()
        settings(startup="blank", content_blocking="off")
        start()
        time.sleep(4)
        check("content_blocking off: no lists fetched",
              "updating filter lists" not in open(log_path).read())
        kill()
    finally:
        kill()
        server.shutdown()
    if failures:
        print(f"kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
