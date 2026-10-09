"""Check printing to PDF, dark pages and page icons.

  python3 test_print_dark_x11.py <path to lrb>

Opens windows on the X11 display ($DISPLAY; XWayland works) for about half
a minute. Serves its own pages (no network). $HOME is a temporary
directory: with no file picker (the session bus is off), PDFs go to its
Downloads folder.

1. The page's icon becomes the window's icon (taskbar, Alt+Tab).
2. Ctrl+P saves the page as a PDF; so does the page's own window.print().
3. Dark pages from the menu: a white page turns dark at once, the choice
   is kept in the site's profile (dark again after a restart) and comes
   off the same way.
"""

import http.server
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11

XK_p, XK_f, XK_Down, XK_Return = 0x70, 0x66, 0xff54, 0xff0d
ControlMask, Mod1Mask = 4, 8


def png(width, height, rgb):
    """A plain PNG of one colour."""
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    raw = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


ICON = png(16, 16, (200, 30, 30))
PAGE = (b"<title>page a</title><link rel=icon href=/icon.png>"
        b"<body style='background:#fff;margin:0'><p>Text to print</p></body>")
served = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        served.append(self.path)
        body, kind = (ICON, "image/png") if self.path == "/icon.png" else (PAGE, "text/html")
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/"
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="print-dark-", dir=runlib.ROOT + "/.profiles")
    home = os.path.join(work, "home")
    downloads = os.path.join(home, "Downloads")
    profile = os.path.join(work, "p")
    os.makedirs(os.path.join(home, ".config", "lrb"))
    with open(os.path.join(home, ".config", "lrb", "settings.json"), "w") as f:
        f.write('{"gpu": false}\n')
    logs = []
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def start():
        before = {w for w, _ in x11.windows()}
        log = os.path.join(work, f"log{len(logs)}")
        logs.append(log)
        proc = subprocess.Popen(
            [binary, f"--user-data-dir={profile}", "--ozone-platform=x11",
             "--remote-debugging-port=0", url],
            stdout=open(log, "wb"), stderr=subprocess.STDOUT,
            env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:", HOME=home,
                     XDG_CONFIG_HOME=os.path.join(home, ".config")),
            start_new_session=True)
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, url, 20)
        deadline = time.monotonic() + 10
        new = []
        while not new and time.monotonic() < deadline:
            new = [w for w, t in x11.windows() if w not in before and t == "page a"]
            time.sleep(0.3)
        return proc, client, session, new[0]

    def brightness(window):
        """The page area's mean brightness, 0-255 (the page is white)."""
        rows = x11.pixels(window, 100, 150, 200, 40)
        values = [sum(p) / 3 for row in rows for p in row]
        return sum(values) / max(1, len(values))

    def wait(predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            time.sleep(0.3)
        return predicate()

    def pdf(name):
        try:
            with open(os.path.join(downloads, name), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return b""

    def menu_dark_pages(window):
        # New tab, New window, Close tab, Print to PDF, Dark pages.
        x11.key(window, XK_f, Mod1Mask)
        time.sleep(1)
        for _ in range(5):
            x11.key(window, XK_Down, 0)
            time.sleep(0.2)
        x11.key(window, XK_Return, 0)

    proc, client, session, window = start()
    try:
        # 1. The icon.
        check("the page's icon is fetched and becomes the window's",
              wait(lambda: x11.icon_size(window) > 0),
              f"served {served}, icon {x11.icon_size(window)} values")

        # 2. PDFs.
        x11.key(window, XK_p, ControlMask)
        check("Ctrl+P saves the page as a PDF",
              wait(lambda: pdf("page a.pdf").startswith(b"%PDF")),
              str(os.listdir(downloads) if os.path.isdir(downloads) else "no Downloads"))
        client.evaluate(session, "window.print(); 1")
        check("the page's window.print() saves one too",
              wait(lambda: pdf("page a (1).pdf").startswith(b"%PDF")),
              str(os.listdir(downloads) if os.path.isdir(downloads) else "no Downloads"))
        text = pdf("page a.pdf")
        check("...a PDF of the page (one page, a font for its text)",
              text.count(b"/Type /Page\n") + text.count(b"/Type /Page ") +
              text.count(b"/Type/Page") >= 1 and b"/Font" in text, f"{len(text)} bytes")

        # 3. Dark pages.
        light = brightness(window)
        menu_dark_pages(window)
        dark = wait(lambda: brightness(window) < 80) and brightness(window)
        scheme = client.evaluate(session, "matchMedia('(prefers-color-scheme: dark)').matches")
        check("Dark pages: the white page turns dark at once, asked for its dark theme",
              light > 200 and dark is not False and scheme,
              f"brightness {light:.0f} -> {brightness(window):.0f}, dark scheme {scheme}")
        check("...kept in the site's profile",
              os.path.exists(os.path.join(profile, "lrb-dark-pages")))
        client.close()
        runlib.stop(proc, None)
        proc, client, session, window = start()
        check("...dark again after a restart",
              wait(lambda: brightness(window) < 80), f"brightness {brightness(window):.0f}")
        menu_dark_pages(window)
        check("...and off the same way",
              wait(lambda: brightness(window) > 200) and
              not os.path.exists(os.path.join(profile, "lrb-dark-pages")),
              f"brightness {brightness(window):.0f}")
        client.close()
    finally:
        runlib.stop(proc, None)
        server.shutdown()
    if failures:
        print(f"kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
