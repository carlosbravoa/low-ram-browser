"""What K open sites cost: site-per-window vs one shared browser.

`instances` mode: K browser instances side by side, one page each, each with
its own profile and cgroup (the site-per-window design). `tabs` mode: one
browser holding the same K pages in K windows (Chrome's model when the
config is multi-process). All K pages are loaded first, then every process
of every instance is sampled at the same moment, so PSS splits the shared
binary pages fairly between instances.
"""

import json
import os
import shutil
import tempfile
import time

from . import cdp as cdplib
from . import configs
from . import procmem
from . import run as runlib


def _open_page(client, session, url, opts):
    client.call("Page.navigate", {"url": url}, session)
    ok = runlib.wait_ready(client, session, url, opts.load_timeout)
    try:
        client.evaluate(session, runlib.SCROLL_JS, await_promise=True)
    except cdplib.CDPError:
        pass
    return ok


def _launch(config, binary, opts, log_path, profiles):
    os.makedirs(opts.profile_dir, exist_ok=True)
    profile = tempfile.mkdtemp(prefix="lrb-profile-", dir=opts.profile_dir)
    profiles.append(profile)
    argv = configs.command_line(config, binary, profile, opts.display, opts.extra_args)
    proc = runlib.launch(argv, log_path, None, opts.cgroup)
    client = cdplib.CDP(runlib.wait_for_devtools(proc, log_path))
    cgroup = procmem.cgroup_dir(proc.pid) if opts.cgroup else None
    return proc, client, cgroup


def _sum(samples):
    keys = procmem.SMAPS_FIELDS + ("processes",)
    return {k: sum(s["totals"].get(k, 0) for s in samples) for k in keys}


def run_one(mode, config, pages, binary, opts, log_prefix):
    record = {"mode": mode, "config": config, "k": len(pages),
              "pages": [p for p, _ in pages], "error": None}
    launched, profiles = [], []
    try:
        if mode == "instances":
            for i, (page, url) in enumerate(pages):
                proc, client, cgroup = _launch(config, binary, opts,
                                               f"{log_prefix}.{i}.log", profiles)
                launched.append((proc, client, cgroup))
                _open_page(client, client.attach_first_page(), url, opts)
        else:
            proc, client, cgroup = _launch(config, binary, opts, f"{log_prefix}.log", profiles)
            launched.append((proc, client, cgroup))
            for i, (page, url) in enumerate(pages):
                if i == 0:
                    session = client.attach_first_page()
                else:
                    target = client.call("Target.createTarget", {"url": "about:blank"})["targetId"]
                    session = client.call("Target.attachToTarget",
                                          {"targetId": target, "flatten": True})["sessionId"]
                _open_page(client, session, url, opts)
        time.sleep(opts.settle)
        samples = [procmem.snapshot(p.pid, cg) for p, _, cg in launched]
        record["totals"] = _sum(samples)
        record["per_instance"] = [s["totals"] for s in samples]
    except Exception as e:  # recorded, not fatal
        record["error"] = f"{type(e).__name__}: {e}"
    finally:
        for proc, client, cgroup in launched:
            client.close()
            runlib.stop(proc, cgroup)
        for profile in profiles:
            shutil.rmtree(profile, ignore_errors=True)
    return record


def run(opts):
    pages = runlib.load_pages(opts.pages_file)
    if opts.pages:
        wanted = opts.pages.split(",")
        pages = sorted((p for p in pages if p[0] in wanted), key=lambda p: wanted.index(p[0]))
    counts = [int(k) for k in opts.counts.split(",")]
    if max(counts) > len(pages):
        raise SystemExit(f"need {max(counts)} pages, have {len(pages)}")
    # "config" runs as separate instances; "config:tabs" as one browser with K windows.
    runs = []
    for spec in opts.configs.split(","):
        config, _, mode = spec.partition(":")
        if config not in configs.CONFIGS:
            raise SystemExit(f"unknown config: {config}")
        runs.append((spec, config, "tabs" if mode == "tabs" else "instances"))
    binaries = runlib.resolve_binaries(opts)

    out = opts.out or os.path.join(runlib.ROOT, "results", "raw",
                                   "multi-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    with open(os.path.join(out, "meta.json"), "w") as f:
        json.dump({"host": runlib.host_info(), "configs": opts.configs, "counts": counts,
                   "pages": pages, "repeats": opts.repeats, "settle": opts.settle,
                   "binaries": binaries}, f, indent=2)
    total, done = opts.repeats * len(counts) * len(runs), 0
    with open(os.path.join(out, "runs.jsonl"), "a") as results:
        for rep in range(opts.repeats):
            for k in counts:
                for spec, config, mode in runs:
                    done += 1
                    binary = binaries[configs.CONFIGS[config]["binary"]]
                    rec = run_one(mode, config, pages[:k], binary, opts,
                                  os.path.join(out, "logs", f"{spec.replace(':', '-')}.k{k}.{rep}"))
                    rec["spec"], rec["repeat"] = spec, rep
                    results.write(json.dumps(rec) + "\n")
                    results.flush()
                    t = rec.get("totals")
                    status = rec["error"] or (
                        f"{(t['Pss_Anon'] + t['Pss_Shmem']) / 1024:.1f} MB unreclaimable, "
                        f"{t['Pss'] / 1024:.1f} MB PSS, {t['processes']} processes")
                    print(f"[{done}/{total}] {spec:30} k={k} {status}", flush=True)
    print(f"results: {out}")
    return out


def report(results_dir):
    import statistics
    runs = [json.loads(l) for l in open(os.path.join(results_dir, "runs.jsonl"))]
    specs = list(dict.fromkeys(r["spec"] for r in runs))
    counts = sorted({r["k"] for r in runs})
    pages = max((r["pages"] for r in runs), key=len)
    lines = [f"# Multi-site cost: `{os.path.basename(results_dir)}`", "",
             f"Pages, in order (k=N uses the first N): {', '.join(pages)}. "
             "Medians in MB; `n` = successful runs.", ""]
    for title, key in (("Unreclaimable PSS (anon + shmem)", lambda t: t["Pss_Anon"] + t["Pss_Shmem"]),
                       ("Total PSS", lambda t: t["Pss"]),
                       ("File-backed PSS", lambda t: t["Pss_File"])):
        lines += [f"## {title}", "", "| k | " + " | ".join(specs) + " |",
                  "|---|" + "---:|" * len(specs)]
        for k in counts:
            cells = []
            for s in specs:
                vals = [key(r["totals"]) / 1024 for r in runs
                        if r["spec"] == s and r["k"] == k and not r["error"]]
                cells.append(f"{statistics.median(vals):.1f}" + ("" if len(vals) > 2 else f" (n={len(vals)})")
                             if vals else "—")
            lines.append(f"| {k} | " + " | ".join(cells) + " |")
        lines.append("")
    errors = [r for r in runs if r["error"]]
    if errors:
        lines += ["## Errors", ""] + [f"- `{r['spec']}` k={r['k']} #{r['repeat']}: {r['error']}"
                                      for r in errors] + [""]
    text = "\n".join(lines)
    with open(os.path.join(results_dir, "report.md"), "w") as f:
        f.write(text)
    return text
