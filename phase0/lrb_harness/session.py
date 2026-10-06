"""A browsing session through lrb_coordinator under a memory cap.

One session: start the coordinator in its own cgroup (all instances are its
children, so they share the cap), open K sites one after another (each as
a user would: a second coordinator invocation hands the URL over), then go
back to each site and check it still works. Sampled after opening all and
after revisiting all.

Variants compare discarding on (the coordinator's default) and off
(both thresholds 0); each instance's own pressure handling stays on.
"""

import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.request

from . import cdp as cdplib
from . import procmem
from . import run as runlib

# The instance configuration of the site-per-window design (lrb-lean-*).
LEAN_ARGS = ["--ozone-platform=headless", "--single-process", "--no-zygote", "--no-sandbox",
             "--in-process-gpu", "--enable-low-end-device-mode", "--disable-gpu",
             "--disable-features=AudioServiceOutOfProcess,BackForwardCache",
             "--enable-features=NetworkServiceInProcess2",
             "--js-flags=--optimize-for-size", "--remote-debugging-port=0"]

VARIANTS = {
    "discard": [],
    "nodiscard": ["--moderate-percent=0", "--critical-percent=0"],
}


def site_of(url):
    """The site the coordinator will file `url` under, for the test pages."""
    host = url.split("://", 1)[1].split("/", 1)[0]
    return ".".join(host.split(".")[-2:])


def instances(profiles):
    """{site: pid} of live lrb browser processes using `profiles`."""
    found = {}
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
        if not args or os.path.basename(args[0]) != "lrb" or \
                f"--lrb-profiles-dir={profiles}" not in args or \
                any(a.startswith("--type=") for a in args):
            continue
        for a in args:
            if a.startswith("--lrb-site="):
                found[a.split("=", 1)[1]] = int(pid)
    return found


def wait_for(predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.5)
    return predicate()


def devtools_port(profile):
    try:
        with open(os.path.join(profile, "DevToolsActivePort")) as f:
            return f.read().split("\n")[0]
    except OSError:
        return None


def page_session(profile, url_prefix):
    """(client, session) for the instance's page whose URL starts with prefix."""
    port = devtools_port(profile)
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/version", timeout=5) as r:
        ws = json.load(r)["webSocketDebuggerUrl"]
    client = cdplib.CDP(ws)
    # lrb opens its first windows after reading any saved state, so DevTools
    # can be up a moment before there is a page.
    deadline = time.monotonic() + 10
    while True:
        targets = client.call("Target.getTargets")["targetInfos"]
        pages = [t for t in targets if t["type"] == "page"]
        if pages or time.monotonic() > deadline:
            break
        time.sleep(0.2)
    if not pages:
        client.close()
        raise RuntimeError("instance has no page")
    target = next((t for t in pages if t["url"].startswith(url_prefix)), pages[0])
    session = client.call("Target.attachToTarget",
                          {"targetId": target["targetId"], "flatten": True})["sessionId"]
    return client, session


def instance_memory(pids):
    out = {}
    for site, pid in pids.items():
        mem = procmem.smaps_rollup(pid)
        if mem:
            out[site] = {"unreclaimable_mb": round((mem["Pss_Anon"] + mem["Pss_Shmem"]) / 1024, 1),
                         "pss_mb": round(mem["Pss"] / 1024, 1)}
    return out


def visit(profile, url, opts):
    """Brings the site's window to the front; True if its page works."""
    try:
        client, session = page_session(profile, url.split("#")[0][:30])
    except Exception:
        return False
    try:
        # A user switching back: the window comes forward and gets a click.
        client.call("Page.bringToFront", {}, session)
        for event in ("mousePressed", "mouseReleased"):
            client.call("Input.dispatchMouseEvent", {"type": event, "x": 5, "y": 5,
                                                     "button": "left", "clickCount": 1},
                        session)
        ok = runlib.wait_ready(client, session, url, opts.load_timeout)
        if ok:
            try:
                client.evaluate(session, runlib.SCROLL_JS, await_promise=True)
            except cdplib.CDPError:
                pass
            title = client.evaluate(session, "document.title")
            here = client.evaluate(session, "location.href")
            ok = bool(title) and not here.startswith("about:")
        return ok
    except Exception:
        return False
    finally:
        client.close()


def run_session(variant, pages, opts, out_dir, rep):
    work = tempfile.mkdtemp(prefix="session-", dir=opts.profile_dir)
    profiles = os.path.join(work, "sites")
    sock = os.path.join(work, "c.sock")
    binary_dir = os.path.abspath(opts.lrb_dir)
    coordinator = [f"{binary_dir}/lrb_coordinator", f"--browser={binary_dir}/lrb",
                   f"--profiles-dir={profiles}", f"--socket={sock}",
                   # Blocking changes page memory: measured separately.
                   "--adblock-setting=off"] + VARIANTS[variant]
    launch = list(coordinator)  # second invocations: just hand a URL over
    coordinator = coordinator + ["--verbose"]
    unit = f"lrb-session-{os.getpid()}-{time.monotonic_ns()}"
    scope = ["systemd-run", "--user", "--scope", "--quiet", f"--unit={unit}",
             "-p", "MemoryAccounting=yes", "-p", "OOMPolicy=continue"]
    if opts.memory_max:
        scope += ["-p", f"MemoryMax={opts.memory_max}", "-p", "MemorySwapMax=0"]
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS="disabled:")
    log_path = os.path.join(out_dir, "logs", f"{variant}.{rep}.log")
    record = {"variant": variant, "repeat": rep, "memory_max": opts.memory_max,
              "pages": [p for p, _ in pages], "opened": {}, "revisited": {}, "error": None}
    log = open(log_path, "wb")
    first_url = pages[0][1]
    t0 = time.monotonic()  # ~ the coordinator's clock start
    proc = subprocess.Popen(scope + ["--"] + coordinator + [first_url, "--"] + LEAN_ARGS,
                            stdout=log, stderr=subprocess.STDOUT, env=env,
                            start_new_session=True)
    cgroup = None
    try:
        for i, (name, url) in enumerate(pages):
            site = site_of(url)
            if i > 0:
                subprocess.run(launch + [url], env=env, timeout=10)
            profile = os.path.join(profiles, site)
            up = wait_for(lambda: site in instances(profiles) and devtools_port(profile), 30)
            if cgroup is None:
                cgroup = procmem.cgroup_dir(proc.pid)
            ok = False
            if up:
                try:
                    client, session = page_session(profile, "http")
                    ok = runlib.wait_ready(client, session, url, opts.load_timeout)
                    try:
                        client.evaluate(session, runlib.SCROLL_JS, await_promise=True)
                    except cdplib.CDPError:
                        pass
                    client.close()
                except Exception:
                    ok = False
            time.sleep(opts.settle)
            record["opened"][name] = ok
            print(f"    {time.monotonic() - t0:7.1f}s opened {name:10} "
                  f"{'ok' if ok else 'FAILED'}", flush=True)

        live = instances(profiles)
        record["after_open"] = {
            "alive": sorted(live), "memory": instance_memory(live),
            "cgroup": procmem.cgroup_stats(cgroup) if cgroup else {}}

        record["reopened"] = {}
        record["outcome"] = {}
        for name, url in pages:
            site = site_of(url)
            profile = os.path.join(profiles, site)
            reopened = site not in instances(profiles)
            with open(log_path, "rb") as f:
                closed = f", closing {site}\n".encode() in f.read()
            if reopened:
                # Closed as a last resort (or killed): open it again, as a
                # user would; a closed site comes back with its history.
                # The closed instance's port file is stale.
                try:
                    os.remove(os.path.join(profile, "DevToolsActivePort"))
                except OSError:
                    pass
                subprocess.run(launch + [url], env=env, timeout=10)
                wait_for(lambda: site in instances(profiles) and devtools_port(profile), 30)
                time.sleep(2)
            ok = site in instances(profiles) and visit(profile, url, opts)
            time.sleep(opts.settle)
            record["revisited"][name] = ok
            record["reopened"][name] = reopened
            # kept: window still there; restored: closed by the coordinator
            # and back with its history; lost: killed (or never opened) and
            # back as a fresh page, history gone; failed: doesn't work.
            outcome = ("failed" if not ok else "kept" if not reopened else
                       "restored" if closed else "lost")
            record["outcome"][name] = outcome
            print(f"    {time.monotonic() - t0:7.1f}s revisit {name:10} {outcome}",
                  flush=True)

        live = instances(profiles)
        record["after_revisit"] = {
            "alive": sorted(live), "memory": instance_memory(live),
            "cgroup": procmem.cgroup_stats(cgroup) if cgroup else {}}
    except Exception as e:  # recorded, not fatal
        record["error"] = f"{type(e).__name__}: {e}"
    finally:
        if cgroup:
            record["cgroup_final"] = procmem.cgroup_stats(cgroup)
            try:
                with open(os.path.join(cgroup, "cgroup.kill"), "w") as f:
                    f.write("1")
            except OSError:
                pass
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, 9)
        log.close()
        with open(log_path, "rb") as f:
            text = f.read()
        record["discards"] = text.count(b"discarding ")
        record["closes"] = text.count(b", closing ")
        shutil.rmtree(work, ignore_errors=True)
    return record


def run(opts):
    pages = runlib.load_pages(opts.pages_file)
    wanted = opts.pages.split(",")
    pages = sorted((p for p in pages if p[0] in wanted), key=lambda p: wanted.index(p[0]))
    variants = opts.variants.split(",")
    out = opts.out or os.path.join(runlib.ROOT, "results", "raw",
                                   "session-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    os.makedirs(opts.profile_dir, exist_ok=True)
    with open(os.path.join(out, "meta.json"), "w") as f:
        json.dump({"host": runlib.host_info(), "pages": pages, "variants": variants,
                   "memory_max": opts.memory_max, "repeats": opts.repeats,
                   "settle": opts.settle, "lrb_dir": os.path.abspath(opts.lrb_dir)}, f, indent=2)
    with open(os.path.join(out, "runs.jsonl"), "a") as results:
        for rep in range(opts.repeats):
            for variant in variants:
                print(f"[{variant} #{rep}] cap {opts.memory_max or 'none'}", flush=True)
                rec = run_session(variant, pages, opts, out, rep)
                results.write(json.dumps(rec) + "\n")
                results.flush()
                print(f"    -> alive after revisit: {rec.get('after_revisit', {}).get('alive')}, "
                      f"discards {rec['discards']}, error {rec['error']}", flush=True)
    print(f"results: {out}")
    return out


def report(results_dir):
    runs = [json.loads(l) for l in open(os.path.join(results_dir, "runs.jsonl"))]
    pages = runs[0]["pages"]
    lines = [f"# Session: `{os.path.basename(results_dir)}`", "",
             f"Cap {runs[0]['memory_max'] or 'none'}. Sites opened in order: "
             f"{', '.join(pages)}; then each revisited. Cells: runs ok / runs.", ""]
    variants = list(dict.fromkeys(r["variant"] for r in runs))
    lines += ["| | " + " | ".join(f"{v}: opened" + f" | {v}: revisited" for v in variants) + " |",
              "|---|" + ":-:|" * (2 * len(variants))]
    for page in pages:
        cells = []
        for v in variants:
            rs = [r for r in runs if r["variant"] == v]
            cells.append(f"{sum(r['opened'].get(page, False) for r in rs)}/{len(rs)}")
            cells.append(f"{sum(r['revisited'].get(page, False) for r in rs)}/{len(rs)}")
        lines.append(f"| {page} | " + " | ".join(cells) + " |")
    lines.append("")
    lines += ["Revisit outcomes over all runs: **kept** (window still there), **restored** "
              "(closed by the coordinator, back with its history), **lost** (killed or never "
              "opened, back as a fresh page), **failed**.", "",
              "| variant | kept | restored | lost | failed |", "|---|:-:|:-:|:-:|:-:|"]
    for v in variants:
        rs = [r for r in runs if r["variant"] == v]
        counts = {k: sum(list(r.get("outcome", {}).values()).count(k) for r in rs)
                  for k in ("kept", "restored", "lost", "failed")}
        lines.append(f"| {v} | {counts['kept']} | {counts['restored']} | {counts['lost']} | "
                     f"{counts['failed']} |")
    lines.append("")
    for v in variants:
        rs = [r for r in runs if r["variant"] == v]
        alive = [len(r.get("after_revisit", {}).get("alive", [])) for r in rs]
        peak = [round(r.get("cgroup_final", {}).get("peak", 0) / 1024) for r in rs]
        discards = [r["discards"] for r in rs]
        closes = [r.get("closes", 0) for r in rs]
        reopened = [sum(r.get("reopened", {}).values()) for r in rs]
        lines.append(f"- **{v}**: windows alive at the end {alive}, discards {discards}, "
                     f"closes {closes}, sites reopened on revisit {reopened}, "
                     f"cgroup peak MB {peak}")
    text = "\n".join(lines) + "\n"
    with open(os.path.join(results_dir, "report.md"), "w") as f:
        f.write(text)
    return text
