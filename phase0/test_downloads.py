"""Check downloads: content_shell cancelled every one on Linux.

  python3 test_downloads.py <path to lrb>

Without a session bus (as here: the harness disables it) there is no file
picker to ask with, so downloads go to the Downloads folder under a free
name. With one, lrb asks where to save with the desktop portal's picker:
test_portal_picker.py. Serves the files itself (no network); HOME is a
temporary folder, so nothing lands in the real Downloads.
"""

import http.server
import os
import sys
import tempfile
import threading
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib

BODY = b"lrb download test\n" * 1000
PDF = b"%PDF-1.4\n% lrb test\n" + b"0" * 2000 + b"\n%%EOF\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/report"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Disposition", 'attachment; filename="report.txt"')
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
        elif self.path.startswith("/paper.pdf"):
            self.send_response(200)
            self.send_header("Content-Type", "application/pdf")
            self.send_header("Content-Length", str(len(PDF)))
            self.end_headers()
            self.wfile.write(PDF)
        else:
            page = b'<a id="r" href="/report">report</a> <a id="p" href="/paper.pdf">paper</a>'
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)

    def log_message(self, *args):
        pass


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="downloads-", dir=runlib.ROOT + "/.profiles")
    home = os.path.join(work, "home")
    downloads = os.path.join(home, "Downloads")
    os.makedirs(home)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"

    os.environ["HOME"] = home  # the Downloads folder lrb picks (XDG fallback)
    os.environ.pop("XDG_CONFIG_HOME", None)
    log = os.path.join(work, "log")
    proc = runlib.launch([binary, f"--user-data-dir={work}/p", "--ozone-platform=headless",
                          "--single-process", "--no-zygote", "--no-sandbox",
                          "--remote-debugging-port=0", base], log, None, False)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    def files():
        return sorted(os.listdir(downloads)) if os.path.isdir(downloads) else []

    def wait_for(predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and not predicate():
            time.sleep(0.3)
        return predicate()

    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)

        client.evaluate(session, "document.getElementById('r').click(); 1")
        got = wait_for(lambda: files() == ["report.txt"])
        check("a download is saved to the Downloads folder (no picker to ask with)",
              got, f"{files()}")
        with open(os.path.join(downloads, "report.txt"), "rb") as f:
            check("...complete, under its own name (no .crdownload left)",
                  f.read() == BODY and not any(n.endswith(".crdownload") for n in files()))

        client.evaluate(session, "document.getElementById('r').click(); 1")
        got = wait_for(lambda: len(files()) == 2)
        check("the same file again gets a free name, nothing overwritten", got, f"{files()}")

        client.evaluate(session, "document.getElementById('p').click(); 1")
        got = wait_for(lambda: "paper.pdf" in files())
        check("a PDF (no viewer in lrb) downloads", got, f"{files()}")
        check("the page stays where it was",
              client.evaluate(session, "location.href") == base,
              client.evaluate(session, "location.href"))
    finally:
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
