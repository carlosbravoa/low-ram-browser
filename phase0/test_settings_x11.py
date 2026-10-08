"""Check the settings window: choosing the search engine.

  python3 test_settings_x11.py <path to lrb>

Opens windows on the X11 display ($DISPLAY; XWayland works) for about half
a minute. Serves its own page; the search itself isn't loaded (it opens
the search engine's window, whose command line says where it goes).
Settings go to a temporary $XDG_CONFIG_HOME, never the user's.

1. Menu (Alt+F), Settings (drawn inside the window, focus on the search
   engine list), Down (the next engine: DuckDuckGo with script), Enter
   (Save): saved, and the rendering choice, untouched, stays unset.
2. A search typed in the bar goes to the chosen engine.
"""

import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11

XK_Down, XK_Return, XK_f, XK_l, ControlMask, Mod1Mask = 0xff54, 0xff0d, 0x66, 0x6c, 4, 8
PAGE = b"<title>start</title>start"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(PAGE)))
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, *args):
        pass


def instance_urls(profiles):
    """The start URLs of lrb instances on `profiles`."""
    urls = []
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
        if f"--lrb-profiles-dir={profiles}" in args and \
                not any(a.startswith("--type=") for a in args):
            urls += [a for a in args if a.startswith("http")]
    return urls


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    work = tempfile.mkdtemp(prefix="settings-", dir=runlib.ROOT + "/.profiles")
    config = os.path.join(work, "config")
    profiles = os.path.join(work, "sites")
    log = os.path.join(work, "log")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/p", f"--lrb-profiles-dir={profiles}",
         "--ozone-platform=x11", "--remote-debugging-port=0", base],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT,
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:", XDG_CONFIG_HOME=config),
        start_new_session=True)  # so runlib.stop() can kill its group
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)
        time.sleep(1)
        window = x11.windows()[0][0]

        x11.key(window, XK_f, Mod1Mask)  # the menu
        time.sleep(0.8)
        for _ in range(4):  # New tab, New window, Close tab, Settings...
            x11.key(window, XK_Down, 0)
            time.sleep(0.3)
        x11.key(window, XK_Return, 0)
        time.sleep(2)
        # The dialog is part of the window: its keys go to the window.
        x11.key(window, XK_Down, 0)  # the search engine list has focus
        time.sleep(0.4)
        x11.key(window, XK_Return, 0)  # Save
        time.sleep(1.5)
        saved = {}
        try:
            with open(os.path.join(config, "lrb", "settings.json")) as f:
                saved = json.load(f)
        except OSError:
            pass
        check("Settings: choosing an engine and saving writes it, rendering stays unchosen",
              saved.get("search_url") == "https://duckduckgo.com/?q=%s" and "gpu" not in saved,
              saved)
        check("...and the new defaults: start on the last site, content blocking automatic",
              saved.get("startup") == "last-site" and "content_blocking" not in saved and
              "startup_page" not in saved, saved)

        x11.key(window, XK_l, ControlMask)
        time.sleep(0.4)
        for c in "lrb test":
            x11.key(window, ord(c) if c != " " else 0x20, 0)
            time.sleep(0.1)
        x11.key(window, XK_Return, 0)
        time.sleep(4)
        check("a search goes to the chosen engine",
              any(u.startswith("https://duckduckgo.com/?q=lrb+test") for u in
                  instance_urls(profiles)), instance_urls(profiles))
        client.close()
    finally:
        runlib.stop(proc, None)
        subprocess.run(["pkill", "-9", "-f", "--", f"--lrb-profiles-dir={profiles}"])
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
