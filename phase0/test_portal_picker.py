"""Check downloads and uploads ask through the desktop portal's picker.

  python3 test_downloads_portal.py <path to lrb>

Needs the user's desktop session: its session bus and xdg-desktop-portal
(file dialogs open on the display for a few seconds). No one is there to
pick or cancel, and nothing can answer the portal's dialog for the user
(Request.Close ends it without a response), so this checks that lrb asks
the portal: SaveFile for a download, OpenFile for an upload (watched with
dbus-monitor). It then closes the requests to clear the dialogs. What
happens on an answer is the same code as without a picker
(test_downloads.py, test_uploads.py) and content's own once a path comes
back.
"""

import http.server
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib
from lrb_harness import x11  # noqa: F401 (sets DISPLAY/XAUTHORITY when unset)
from test_downloads import Handler


def close_requests(dbus_log):
    """Ends the portal requests in `dbus_log`: closes their dialogs."""
    for handle in set(re.findall(
            r'object path "(/org/freedesktop/portal/desktop/request/[^"]+)"', dbus_log)):
        subprocess.run(["gdbus", "call", "--session", "--dest",
                        "org.freedesktop.portal.Desktop", "--object-path", handle,
                        "--method", "org.freedesktop.portal.Request.Close"],
                       capture_output=True, timeout=10)


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="dlportal-", dir=runlib.ROOT + "/.profiles")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}/"
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    monitor_log = os.path.join(work, "dbus-monitor")
    monitor = subprocess.Popen(
        ["dbus-monitor", "--session",
         "type='method_call',interface='org.freedesktop.portal.FileChooser'",
         "type='method_return'"],
        stdout=open(monitor_log, "w"), stderr=subprocess.DEVNULL)
    log = os.path.join(work, "log")
    # The real session bus this time (runlib.launch would disable it).
    proc = subprocess.Popen(
        [binary, f"--user-data-dir={work}/p", "--ozone-platform=x11",
         "--remote-debugging-port=0", "--single-process", "--no-zygote", "--no-sandbox", base],
        stdout=open(log, "wb"), stderr=subprocess.STDOUT, start_new_session=True)
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, base, 20)
        time.sleep(1)
        client.evaluate(session, "document.getElementById('r').click(); 1")
        time.sleep(4)
        text = open(monitor_log).read()
        check("the download asks the portal where to save (SaveFile)",
              "member=SaveFile" in text, "" if "member=SaveFile" in text else "no SaveFile call seen")

        close_requests(text)

        # Upload: <input type=file> asks the portal to open a file.
        client.evaluate(session, """{ const f = document.createElement('input');
            f.type = 'file'; f.id = 'f'; document.body.appendChild(f);
            } 1""")
        open(monitor_log, "w").close()
        client.call("Runtime.evaluate", {"expression": "f.click(); 1", "userGesture": True},
                    session)
        time.sleep(4)
        text = open(monitor_log).read()
        check("choosing a file to upload asks the portal (OpenFile)",
              "member=OpenFile" in text, "" if "member=OpenFile" in text else "not seen")
        close_requests(text)
        client.close()
    finally:
        runlib.stop(proc, None)
        monitor.terminate()
        server.shutdown()
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
