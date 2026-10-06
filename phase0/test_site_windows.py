"""Check lrb's one-site-per-window rule end to end.

  python3 test_site_windows.py <path to lrb> [--keep]

Starts one lrb instance, drives it over DevTools, and checks which
navigations stay in the window and which open another site's window (a new
lrb process with --lrb-site=<site>). Needs network access.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib


def instance_args(profiles):
    """Command lines of lrb browser processes using our profiles dir."""
    found = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                argv = f.read().split(b"\0")
        except OSError:
            continue
        args = [a.decode(errors="replace") for a in argv if a]
        if len(args) == 1:
            args = args[0].split()  # Chromium rewrites it as one string
        if any(a == f"--lrb-profiles-dir={profiles}" for a in args) and \
                not any(a.startswith("--type=") for a in args):
            found.append(args)
    return found


def site_processes(profiles):
    """--lrb-site values of lrb browser processes using our profiles dir."""
    return sorted(a.split("=", 1)[1] for args in instance_args(profiles)
                  for a in args if a.startswith("--lrb-site="))


def main():
    binary = os.path.abspath(sys.argv[1])
    # On disk, not /tmp: a tmpfs profile turns disk caches into RAM.
    os.makedirs(runlib.ROOT + "/.profiles", exist_ok=True)
    work = tempfile.mkdtemp(prefix="lrb-site-test-", dir=runlib.ROOT + "/.profiles")
    profiles = os.path.join(work, "sites")
    log = os.path.join(work, "first.log")
    argv = [binary, f"--user-data-dir={work}/first", f"--lrb-profiles-dir={profiles}",
            "--remote-debugging-port=0", "--ozone-platform=headless",
            "--single-process", "--no-zygote", "--no-sandbox",
            "--vmodule=site_window_throttle=1", "about:blank"]
    proc = runlib.launch(argv, log, None, use_cgroup=False)
    failures = 0
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()

        def go(url):
            client.call("Page.navigate", {"url": url}, session)
            runlib.wait_ready(client, session, url, 20)
            time.sleep(1)
            return client.evaluate(session, "location.hostname")

        def check(name, ok, detail):
            nonlocal failures
            failures += not ok
            print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")

        host = go("https://en.wikipedia.org/wiki/Web_browser")
        check("first navigation sets the site", host.endswith("wikipedia.org"), host)

        pages = lambda: [t for t in client.call("Target.getTargets")["targetInfos"]
                         if t["type"] == "page"]

        def open_page(url):
            """Another wikipedia.org page (window) of this instance."""
            target = client.call("Target.createTarget", {"url": url})["targetId"]
            sess = client.call("Target.attachToTarget",
                               {"targetId": target, "flatten": True})["sessionId"]
            runlib.wait_ready(client, sess, url, 20)
            time.sleep(1)
            return target, sess

        def gone(target):
            time.sleep(4)
            return not any(t["targetId"] == target for t in pages())

        def bounds_of(site):
            """--lrb-window-bounds given to site's instance, if any."""
            for args in instance_args(profiles):
                if f"--lrb-site={site}" in args:
                    return next((a.split("=", 1)[1] for a in args
                                 if a.startswith("--lrb-window-bounds=")), None)
            return None

        # Leaving the site replaces the window: typed addresses and plain
        # links to another site open its window in this one's place, and
        # this one closes. The first page keeps the instance alive.
        target, sess = open_page("https://en.wikipedia.org/wiki/Main_Page")
        client.call("Page.navigate", {"url": "https://github.com/"}, sess)
        check("typed cross-site URL: this window moves on (closes)", gone(target),
              f"pages: {[t['url'] for t in pages()]}")
        check("...github.com's window opens in its place",
              "github.com" in site_processes(profiles) and bounds_of("github.com"),
              f"site windows: {site_processes(profiles)}, bounds {bounds_of('github.com')}")

        go("https://en.wikipedia.org/wiki/Main_Page")
        client.evaluate(session, "location.href = 'https://example.org/'; 1")
        time.sleep(4)
        host = client.evaluate(session, "location.hostname")
        check("script navigation passes through (flow)", host == "example.org", host)
        go("https://en.wikipedia.org/wiki/Main_Page")

        add_link = """
          { const a = document.createElement('a');
            a.href = 'https://www.iana.org/'; a.id = 'lrb-test-link';
            a.textContent = 'IANA'; a.style = 'position:fixed;left:10px;top:10px;font-size:40px;z-index:99999';
            document.body.appendChild(a); } 1"""
        target, sess = open_page("https://en.wikipedia.org/wiki/Web_browser")
        client.evaluate(sess, add_link)
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 30, "y": 30,
                                                     "button": "left", "clickCount": 1}, sess)
        check("link click to another site: this window moves on (closes)", gone(target),
              f"pages: {[t['url'] for t in pages()]}")
        check("...iana.org's window opens in its place",
              "iana.org" in site_processes(profiles) and bounds_of("iana.org"),
              f"site windows: {site_processes(profiles)}, bounds {bounds_of('iana.org')}")

        client.evaluate(session, add_link)
        # A target=_blank link to another site: the new window's first page
        # goes to that site's window, and the new window, left empty, closes.
        before = len(pages())
        client.evaluate(session, """
          { const b = document.getElementById('lrb-test-link');
            b.href = 'https://www.w3.org/'; b.target = '_blank'; } 1""")
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 30, "y": 30,
                                                     "button": "left", "clickCount": 1}, session)
        time.sleep(5)
        check("new-window link to another site opens w3.org's window (a new one)",
              "w3.org" in site_processes(profiles) and not bounds_of("w3.org"),
              f"site windows: {site_processes(profiles)}")
        check("...and leaves no empty window behind", len(pages()) == before,
              f"{before} page(s) before, {len(pages())} after")

        # A sign-in style popup: window.open on a click, with an opener to
        # report back to. A flow: it loads here, in a popup.
        client.evaluate(session, """
          { const b = document.getElementById('lrb-test-link');
            b.removeAttribute('target'); b.href = '#';
            b.onclick = e => { e.preventDefault(); window.lrbPopup = window.open('https://example.com/', 'login', 'width=400,height=400'); }; } 1""")
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 30, "y": 30,
                                                     "button": "left", "clickCount": 1}, session)
        time.sleep(5)
        popup = [t["url"] for t in pages() if "example.com" in t["url"]]
        check("popup with an opener (sign-in flow) loads in this window",
              bool(popup) and "example.com" not in site_processes(profiles),
              f"pages: {[t['url'] for t in pages()]}, site windows: {site_processes(profiles)}")
        client.evaluate(session, "window.lrbPopup && window.lrbPopup.close(); 1")
        time.sleep(1)

        # A window opened by script alone, without opener, to another site:
        # nobody asked for it.
        before = len(pages())
        client.evaluate(session, "window.open('https://www.ietf.org/', '_blank', 'noopener'); 1")
        time.sleep(5)
        check("unrequested noopener window to another site is blocked",
              "ietf.org" not in site_processes(profiles) and len(pages()) == before,
              f"{before} page(s) before, {len(pages())} after, site windows: {site_processes(profiles)}")
        client.close()
    finally:
        runlib.stop(proc, None)
        subprocess.run(["pkill", "-f", "--", f"--lrb-profiles-dir={profiles}"])
        time.sleep(1)
        if "--keep" in sys.argv:
            print(f"kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
