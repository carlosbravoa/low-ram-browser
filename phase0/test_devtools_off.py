"""Check that lrb listens on no network port unless asked to.

  python3 test_devtools_off.py <path to lrb>

content_shell always started DevTools' remote debugging server (an
ephemeral port on loopback when no --remote-debugging-port was given):
any local process could then drive every window, a bank's included. lrb
starts it only with --remote-debugging-port or --remote-debugging-pipe
(tests, the harness). Starts lrb plainly on a local page, then checks that
no socket of its process is listening and that no DevToolsActivePort file
was written; then that --remote-debugging-port still works.
"""

import os
import sys
import tempfile
import time

from lrb_harness import run as runlib


def listening_ports(pid):
    """TCP ports the process listens on (IPv4 and IPv6)."""
    inodes = set()
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    ports = []
    for table in ("tcp", "tcp6"):
        with open(f"/proc/{pid}/net/{table}") as f:
            next(f)
            for line in f:
                fields = line.split()
                if fields[3] == "0A" and fields[9] in inodes:  # LISTEN
                    ports.append(int(fields[1].rsplit(":", 1)[1], 16))
    return ports


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="devtools-", dir=runlib.ROOT + "/.profiles")
    page = "file://" + os.path.join(runlib.PHASE0, "pages", "article.html")
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    proc = runlib.launch([binary, f"--user-data-dir={work}/plain",
                          "--ozone-platform=headless", page],
                         os.path.join(work, "plain.log"), None, False)
    try:
        time.sleep(6)
        check("lrb is running", proc.poll() is None)
        ports = listening_ports(proc.pid)
        check("no listening port without --remote-debugging-port", not ports,
              f"listening on {ports}")
        check("no DevToolsActivePort file",
              not os.path.exists(os.path.join(work, "plain", "DevToolsActivePort")))
    finally:
        runlib.stop(proc, None)

    log = os.path.join(work, "asked.log")
    proc = runlib.launch([binary, f"--user-data-dir={work}/asked",
                          "--ozone-platform=headless", "--remote-debugging-port=0", page],
                         log, None, False)
    try:
        runlib.wait_for_devtools(proc, log)
        ports = listening_ports(proc.pid)
        check("--remote-debugging-port=0 listens", len(ports) == 1, f"listening on {ports}")
    except Exception as e:  # no DevTools at all
        check("--remote-debugging-port=0 listens", False, repr(e))
    finally:
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
