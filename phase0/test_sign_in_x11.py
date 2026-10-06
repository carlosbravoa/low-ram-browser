"""Check the sign-in prompt for sites using HTTP authentication
(content_shell cancelled it: such sites couldn't be used).

  python3 test_sign_in_x11.py <path to lrb>

Opens a window on the X11 display ($DISPLAY; XWayland works) for about
half a minute. Serves its own pages (no network). Keys through X11
(lrb_harness/x11.py).

1. A page asking for a password: the prompt takes focus on the username;
   username, Enter, password, Enter signs in.
2. Escape cancels: the server's refusal page shows.
"""

import base64
import http.server
import os
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11

XK_Return, XK_Escape = 0xff0d, 0xff1b
GOOD = "Basic " + base64.b64encode(b"user:secret").decode()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith(("/private", "/other")):
            # Two protected areas: credentials given for one are reused there
            # (the HTTP auth cache), not for the other.
            realm = "private" if self.path.startswith("/private") else "other"
            if self.headers.get("Authorization") == GOOD:
                self.reply(200, b"<title>welcome</title>welcome")
            else:
                self.reply(401, b"<title>refused</title>refused",
                           {"WWW-Authenticate": f'Basic realm="{realm}"'})
        else:
            self.reply(200, b"<title>start</title>start")

    def reply(self, code, body, headers=None):
        self.send_response(code)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    work = tempfile.mkdtemp(prefix="signin-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
         "--remote-debugging-port=0", "--single-process", "--no-zygote", "--no-sandbox", base],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT,
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:"),
        start_new_session=True)  # so runlib.stop() can kill its group
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)
        time.sleep(1)
        window = x11.windows()[0][0]

        def type_text(text):
            for c in text:
                x11.key(window, ord(c), 0)
                time.sleep(0.15)

        def title():
            for _ in range(20):
                try:
                    value = client.evaluate(session, "document.title")
                    if value not in ("", "start"):
                        return value
                except cdplib.CDPError:
                    pass
                time.sleep(0.5)
            return client.evaluate(session, "document.title")

        client.evaluate(session, f"location.href = '{base}private/1'; 1")
        time.sleep(1.5)
        type_text("user")
        x11.key(window, XK_Return, 0)
        time.sleep(0.3)
        type_text("secret")
        x11.key(window, XK_Return, 0)
        check("username, Enter, password, Enter signs in", title() == "welcome", title())

        client.evaluate(session, f"location.href = '{base}other/1'; 1")
        time.sleep(1.5)
        x11.key(window, XK_Escape, 0)
        check("Escape cancels: the site's refusal shows", title() == "refused", title())
        client.close()
    finally:
        runlib.stop(proc, None)
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
