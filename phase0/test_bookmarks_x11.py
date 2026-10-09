"""Check bookmarks and the address bar's suggestions.

  python3 test_bookmarks_x11.py <dir with lrb and lrb_coordinator>

Through the coordinator (it keeps bookmarks for all sites), with windows on
the X11 display ($DISPLAY; XWayland works) for about half a minute. Local
pages only; profiles and settings in a temporary directory.

1. Ctrl+D bookmarks the page (with its title); again, removes it.
2. Typing a bookmark's title word offers it (a list under the address):
   Down, Enter opens it.
3. Typing the start of an address completes it inline: Enter opens the
   bookmark (http, as kept: the completion shows no scheme).
4. The menu's Bookmarks submenu opens one.
5. Only an instance may add bookmarks, and only its own site's pages: a
   connection that isn't one is refused.
"""

import http.server
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import x11
from test_coordinator import devtools, instances, wait_for
from test_discard import LEAN_ARGS

XK_d, XK_l, XK_f = 0x64, 0x6c, 0x66
XK_Down, XK_Right, XK_Return = 0xff54, 0xff53, 0xff0d
ControlMask, Mod1Mask = 4, 8
TITLES = {"a": "page a", "b": "bread recipes", "c": "page c"}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        name = self.path.strip("/") or "a"
        body = f"<title>{TITLES.get(name, name)}</title>{name}".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    out = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="lrb-bookmarks-", dir="/tmp")
    profiles = os.path.join(work, "sites")
    profile = os.path.join(profiles, "127.0.0.1")
    os.makedirs(os.path.join(work, "config", "lrb"))
    with open(os.path.join(work, "config", "lrb", "settings.json"), "w") as f:
        f.write('{"gpu": false}\n')
    bookmarks_file = os.path.join(profiles, ".bookmarks")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"

    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def bookmarks():
        try:
            with open(bookmarks_file) as f:
                return [line.split("\t") for line in f.read().splitlines()]
        except FileNotFoundError:
            return []

    before = {w for w, _ in x11.windows()}
    log = open(os.path.join(work, "coordinator.log"), "w")
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:",
               XDG_CONFIG_HOME=os.path.join(work, "config"))
    coordinator = subprocess.Popen(
        [f"{out}/lrb_coordinator", f"--browser={out}/lrb", "--verbose",
         f"--profiles-dir={profiles}", f"--socket={work}/c.sock",
         "--adblock-setting=off", base + "a", "--"] +
        [a for a in LEAN_ARGS if not a.startswith("--ozone-platform")],
        stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    try:
        new = wait_for(lambda: [w for w, t in x11.windows()
                                if w not in before and t == "page a"], 30)
        if not new:
            check("the site's window opens", False)
            return 1
        window = new[0]
        client = devtools(profile)
        session = client.attach_first_page()

        def go(name):
            client.evaluate(session, f"location.href = '{base}{name}'; 1")
            wait_for(lambda: x11.title(window) == TITLES[name], 10)
            time.sleep(1)  # the star learns whether it's bookmarked

        def at():
            wait_for(lambda: client.evaluate(session, "document.readyState") == "complete", 10)
            return client.evaluate(session, "location.href")

        # 1. Ctrl+D, twice.
        x11.key(window, XK_d, ControlMask)
        check("Ctrl+D bookmarks the page, with its title",
              wait_for(lambda: [base + "a", "page a"] in bookmarks(), 5), str(bookmarks()))
        go("b")
        x11.key(window, XK_d, ControlMask)
        wait_for(lambda: any(b[0] == base + "b" for b in bookmarks()), 5)
        time.sleep(1)
        x11.key(window, XK_d, ControlMask)
        check("Ctrl+D again removes it",
              wait_for(lambda: not any(b[0] == base + "b" for b in bookmarks()), 5),
              str(bookmarks()))
        time.sleep(1)
        x11.key(window, XK_d, ControlMask)
        wait_for(lambda: any(b[0] == base + "b" for b in bookmarks()), 5)

        # 2. A title word: offered, Down, Enter.
        go("c")
        # The list is drawn in the window, over the page (white, its text
        # at the top left): marks under the address are the list's.
        def list_marks():
            rows = x11.pixels(window, 250, 45, 400, 80)
            return sum(1 for row in rows for p in row if sum(p) < 600)

        x11.key(window, XK_l, ControlMask)
        time.sleep(0.5)
        x11.type_text(window, "bread")
        popup = wait_for(lambda: list_marks() > 100, 5)
        check("typing offers suggestions (a list under the address)", bool(popup),
              f"{list_marks()} marks under the address")
        x11.key(window, XK_Down, 0)
        x11.key(window, XK_Return, 0)
        time.sleep(1.5)
        check("Down, Enter opens the bookmark whose title matched",
              at() == base + "b", at())
        check("...and the list closes", wait_for(lambda: list_marks() == 0, 5),
              f"{list_marks()} marks under the address")

        # 3. Inline completion of an address: the first bookmark (a).
        go("c")
        x11.key(window, XK_l, ControlMask)
        time.sleep(0.5)
        x11.type_text(window, "127.0")
        time.sleep(1)
        x11.key(window, XK_Return, 0)
        time.sleep(1.5)
        check("the address typed is completed inline, Enter opens it (http)",
              at() == base + "a", at())

        # 4. The menu's Bookmarks submenu: New tab, New window, Close tab,
        # Bookmarks.
        go("c")
        x11.key(window, XK_f, Mod1Mask)
        time.sleep(1)
        for _ in range(4):
            x11.key(window, XK_Down, 0)
            time.sleep(0.2)
        x11.key(window, XK_Right, 0)
        time.sleep(0.5)
        x11.key(window, XK_Return, 0)
        time.sleep(1.5)
        check("the menu's Bookmarks opens one", at() == base + "a", at())
        client.close()

        # 5. Not an instance: refused.
        count = len(bookmarks())
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.connect(f"{work}/c.sock")
            s.sendall(f"bookmark-add {base}evil x\nbookmark-remove {base}a\n".encode())
            time.sleep(1)
        check("a connection that isn't an instance can't add or remove",
              len(bookmarks()) == count and not any(b[0] == base + "evil" for b in bookmarks()),
              str(bookmarks()))
    finally:
        os.killpg(coordinator.pid, 15)
        coordinator.wait(10)
        server.shutdown()
        log.close()
    if failures:
        print(f"kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
