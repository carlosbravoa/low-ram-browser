"""Run configurations x pages, sampling memory at fixed points of each visit.

One run = fresh profile, launch, navigate, scroll, settle, sample, pressure,
sample, kill. Each run gets its own cgroup (a systemd user scope) so the
whole browser, including every child process, can be accounted and capped.
"""

import json
import os
import platform
import re
import shutil
import signal
import subprocess
import tempfile
import time

from . import cdp as cdplib
from . import configs
from . import procmem

PHASE0 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(PHASE0)

# Walk the page a screen at a time so lazy-loaded content and images load,
# then return to the top, as a reader would leave it.
SCROLL_JS = """
(async () => {
  const el = document.scrollingElement || document.documentElement;
  for (let i = 0; i < 20 && innerHeight * i < el.scrollHeight; i++) {
    scrollBy(0, innerHeight);
    await new Promise(r => setTimeout(r, 300));
  }
  scrollTo(0, 0);
  return el.scrollHeight;
})()
"""

DEVTOOLS_RE = re.compile(rb"DevTools listening on (ws://\S+)")


def load_pages(path):
    pages = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, url = line.split(None, 1)
            if url.startswith("file://") and not url.startswith("file:///"):
                url = "file://" + os.path.join(os.path.dirname(os.path.abspath(path)), url[7:])
            pages.append((name, url))
    return pages


def launch(argv, log_path, memory_max, use_cgroup):
    # No session bus for the browser. With one, Chromium asks systemd to move
    # it into its own app-*.scope, escaping our cgroup (and any memory cap)
    # moments after startup. The target devices won't run a desktop bus either.
    argv = ["env", "DBUS_SESSION_BUS_ADDRESS=disabled:"] + argv
    if use_cgroup:
        unit = f"lrb-{os.getpid()}-{time.monotonic_ns()}"
        prefix = ["systemd-run", "--user", "--scope", "--quiet", f"--unit={unit}",
                  "-p", "MemoryAccounting=yes"]
        if memory_max:
            # No swap either: on the target, overflow means the OOM killer.
            prefix += ["-p", f"MemoryMax={memory_max}", "-p", "MemorySwapMax=0"]
        argv = prefix + ["--"] + argv
    with open(log_path, "wb") as log:
        return subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)


def wait_for_devtools(proc, log_path, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with open(log_path, "rb") as f:
            m = DEVTOOLS_RE.search(f.read())
        if m:
            return m.group(1).decode()
        if proc.poll() is not None:
            raise RuntimeError(f"browser exited with {proc.returncode} before DevTools came up")
        time.sleep(0.1)
    raise TimeoutError("DevTools endpoint did not appear")


def wait_ready(client, session, url, timeout):
    """True once the new document finished loading; False on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            state = client.evaluate(session, "[document.readyState, document.URL]")
        except cdplib.CDPError:
            state = None  # context swapped mid-navigation
        if state and state[0] == "complete" and (url == "about:blank" or state[1] != "about:blank"):
            return True
        time.sleep(0.5)
    return False


def page_metrics(client, session):
    out = {}
    try:
        out["js_heap"] = client.call("Runtime.getHeapUsage", session_id=session)
    except cdplib.CDPError:
        pass
    try:
        out["dom"] = client.call("Memory.getDOMCounters", session_id=session)
    except cdplib.CDPError:
        pass
    return out


def stop(proc, cgroup):
    if cgroup:
        try:
            with open(os.path.join(cgroup, "cgroup.kill"), "w") as f:
                f.write("1")
        except OSError:
            pass
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        pass


# Smoothness (--perf): a 3 s smooth scroll driven by requestAnimationFrame,
# timing every frame. At 60 Hz a frame is 16.7 ms; one over 25 ms missed at
# least one refresh (jank). Meaningful on a real display (--display native):
# headless frames aren't paced by a screen.
SCROLL_PERF_JS = """
new Promise(resolve => {
  window.scrollTo(0, 0);
  const times = [];
  let last = null;
  const end = performance.now() + 3000;
  function frame(now) {
    if (last !== null) times.push(now - last);
    last = now;
    window.scrollBy(0, 6);
    if (now < end) { requestAnimationFrame(frame); return; }
    times.sort((a, b) => a - b);
    const total = times.reduce((a, b) => a + b, 0);
    resolve({frames: times.length,
             fps: times.length ? 1000 * times.length / total : 0,
             jank_percent: times.length ? 100 * times.filter(t => t > 25).length / times.length : 0,
             p95_ms: times.length ? times[Math.floor(times.length * 0.95)] : 0});
  }
  requestAnimationFrame(frame);
})
"""

# Video (--perf, pages with a <video>): plays the first video muted for 10 s
# (started as if clicked: lrb's policy blocks autoplay) and reads the
# browser's own counters.
VIDEO_PERF_JS = """
(async () => {
  const v = document.querySelector('video');
  if (!v) return null;
  v.muted = true;
  try { await v.play(); } catch (e) { return {error: String(e)}; }
  const q0 = v.getVideoPlaybackQuality();
  await new Promise(r => setTimeout(r, 10000));
  const q1 = v.getVideoPlaybackQuality();
  const total = q1.totalVideoFrames - q0.totalVideoFrames;
  const dropped = q1.droppedVideoFrames - q0.droppedVideoFrames;
  v.pause();
  return {frames: total, fps: total / 10,
          dropped_percent: total ? 100 * dropped / total : null,
          height: v.videoHeight};
})()
"""


def measure_perf(client, session):
    perf = {}
    try:
        perf["scroll"] = client.evaluate(session, SCROLL_PERF_JS, await_promise=True)
    except cdplib.CDPError as e:
        perf["scroll_error"] = str(e)
    try:
        reply = client.call("Runtime.evaluate", {
            "expression": VIDEO_PERF_JS, "awaitPromise": True, "returnByValue": True,
            "userGesture": True}, session)
        value = reply.get("result", {}).get("value")
        if value is not None:
            perf["video"] = value
    except Exception as e:  # noqa: BLE001 (a page without video is fine)
        perf["video_error"] = str(e)
    return perf


def run_one(config, page, url, binary_path, opts, log_path):
    # Not /tmp: where /tmp is tmpfs the profile's disk caches would be RAM and
    # silently inflate the cgroup's shmem charge.
    os.makedirs(opts.profile_dir, exist_ok=True)
    profile = tempfile.mkdtemp(prefix="lrb-profile-", dir=opts.profile_dir)
    argv = configs.command_line(config, binary_path, profile, opts.display, opts.extra_args)
    record = {"config": config, "page": page, "url": url, "argv": argv,
              "started": time.strftime("%Y-%m-%dT%H:%M:%S"), "error": None, "samples": {}}
    proc, cgroup, client = None, None, None
    try:
        proc = launch(argv, log_path, opts.memory_max, opts.cgroup)
        ws_url = wait_for_devtools(proc, log_path)
        if opts.cgroup:
            cgroup = procmem.cgroup_dir(proc.pid)
        client = cdplib.CDP(ws_url)
        session = client.attach_first_page()
        time.sleep(2)
        record["samples"]["startup"] = procmem.snapshot(proc.pid, cgroup)

        t0 = time.monotonic()
        nav = client.call("Page.navigate", {"url": url}, session)
        if nav.get("errorText"):
            raise RuntimeError(f"navigation failed: {nav['errorText']}")
        record["load_complete"] = wait_ready(client, session, url, opts.load_timeout)
        record["load_seconds"] = round(time.monotonic() - t0, 2)
        try:
            record["scroll_height"] = client.evaluate(session, SCROLL_JS, await_promise=True)
        except cdplib.CDPError as e:
            record["scroll_error"] = str(e)
        time.sleep(opts.settle)
        # Where the page really ended up: e.g. Google answers heavy automated
        # traffic with google.com/sorry instead of YouTube, which would
        # otherwise pass for a (small) YouTube page.
        try:
            record["final_url"] = client.evaluate(session, "location.href")
        except cdplib.CDPError:
            pass
        sample = procmem.snapshot(proc.pid, cgroup)
        sample.update(page_metrics(client, session))
        record["samples"]["loaded"] = sample
        if getattr(opts, "perf", False):
            record["perf"] = measure_perf(client, session)

        # How much is reclaimable when the system asks: what the browser can
        # give back under memory pressure is as relevant as what it holds.
        client.call("HeapProfiler.collectGarbage", session_id=session)
        client.call("Memory.simulatePressureNotification", {"level": "critical"}, session)
        time.sleep(3)
        sample = procmem.snapshot(proc.pid, cgroup)
        sample.update(page_metrics(client, session))
        record["samples"]["pressured"] = sample
    except Exception as e:  # recorded, not fatal: one bad site must not end the sweep
        record["error"] = f"{type(e).__name__}: {e}"
        # systemd removes the scope's cgroup once its last process is gone, so
        # a browser killed outright (the OOM killer under --memory-max, in
        # practice) shows up as a vanished cgroup, and its oom_kill count is
        # lost with it.
        if cgroup and not os.path.isdir(cgroup):
            record["error"] = "browser died (cgroup gone; under a cap, an OOM kill): " + record["error"]
            cgroup = None
    finally:
        if cgroup:
            record["cgroup_final"] = procmem.cgroup_stats(cgroup)
        if client:
            client.close()
        if proc:
            stop(proc, cgroup)
        shutil.rmtree(profile, ignore_errors=True)
    return record


def host_info():
    info = {"kernel": platform.release(), "machine": platform.machine(),
            "python": platform.python_version()}
    try:
        with open("/proc/meminfo") as f:
            info["MemTotal_kB"] = int(f.readline().split()[1])
        with open("/proc/cpuinfo") as f:
            models = [l.split(":", 1)[1].strip() for l in f if l.startswith("model name")]
        info["cpu"] = models[0] if models else None
        info["cpus"] = len(models)
    except OSError:
        pass
    return info


def filesystem_type(path):
    os.makedirs(path, exist_ok=True)
    best, fstype = "", None
    with open("/proc/self/mounts") as f:
        for line in f:
            _, mountpoint, kind = line.split()[:3]
            real = os.path.realpath(path)
            if (real == mountpoint or real.startswith(mountpoint.rstrip("/") + "/")) \
                    and len(mountpoint) > len(best):
                best, fstype = mountpoint, kind
    return fstype


def resolve_binaries(opts):
    paths = {}
    for name, rel in configs.PREBUILT_LAYOUT.items():
        paths[name] = os.path.join(opts.prebuilt, rel)
    for override in opts.binary:
        name, path = override.split("=", 1)
        paths[name] = os.path.abspath(path)
    return paths


def build_info(names, binaries):
    """What each binary used was built from: build/build.sh leaves CHROMIUM
    (release and commit) and COMMIT (the patched checkout's head) beside it."""
    info = {}
    for name in sorted(names):
        d = os.path.dirname(binaries[name])
        lines = []
        for f in ("CHROMIUM", "COMMIT"):
            try:
                with open(os.path.join(d, f)) as fh:
                    lines.append(fh.read().strip())
            except OSError:
                pass
        if lines:
            info[name] = " / ".join(lines)
    return info


def run(opts):
    pages = load_pages(opts.pages_file)
    if opts.pages:
        wanted = opts.pages.split(",")
        pages = [p for p in pages if p[0] in wanted]
    names = opts.configs.split(",") if opts.configs else configs.DEFAULT_SET
    unknown = [n for n in names if n not in configs.CONFIGS]
    if unknown:
        raise SystemExit(f"unknown configs: {', '.join(unknown)}")
    binaries = resolve_binaries(opts)
    for n in names:
        b = binaries.get(configs.CONFIGS[n]["binary"])
        if not b:
            raise SystemExit(f"{n}: needs --binary {configs.CONFIGS[n]['binary']}=<path>")
        if not os.access(b, os.X_OK):
            raise SystemExit(f"{n}: binary not found: {b} (run phase0/fetch_prebuilt.sh)")
    if opts.memory_max and not opts.cgroup:
        raise SystemExit("--memory-max needs cgroups")

    out = opts.out or os.path.join(ROOT, "results", "raw", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    meta = {"host": host_info(), "configs": names, "pages": pages, "repeats": opts.repeats,
            "display": opts.display, "memory_max": opts.memory_max, "settle": opts.settle,
            "binaries": binaries, "extra_args": opts.extra_args,
            "profile_dir": opts.profile_dir, "profile_fs": filesystem_type(opts.profile_dir)}
    try:
        with open(os.path.join(opts.prebuilt, "REVISIONS")) as f:
            meta["chromium"] = json.load(f)
    except OSError:
        pass
    meta["builds"] = build_info({configs.CONFIGS[n]["binary"] for n in names}, binaries)
    with open(os.path.join(out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)

    total = opts.repeats * len(pages) * len(names)
    done = 0
    with open(os.path.join(out, "runs.jsonl"), "a") as results:
        # Configs interleaved per page so live-site drift hits all of them alike.
        for rep in range(opts.repeats):
            for page, url in pages:
                for name in names:
                    done += 1
                    log_path = os.path.join(out, "logs", f"{name}.{page}.{rep}.log")
                    rec = run_one(name, page, url, binaries[configs.CONFIGS[name]["binary"]],
                                  opts, log_path)
                    rec["repeat"] = rep
                    results.write(json.dumps(rec) + "\n")
                    results.flush()
                    pss = rec["samples"].get("loaded", {}).get("totals", {}).get("Pss")
                    status = rec["error"] or (f"{pss / 1024:.1f} MB PSS" if pss else "no sample")
                    print(f"[{done}/{total}] {name:24} {page:12} {status}", flush=True)
    print(f"results: {out}")
    return out
