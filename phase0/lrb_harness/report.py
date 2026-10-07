"""Summarize runs.jsonl into Markdown tables (medians, MB)."""

import json
import os
import statistics


def load(results_dir):
    with open(os.path.join(results_dir, "runs.jsonl")) as f:
        return [json.loads(line) for line in f if line.strip()]


def mb(kib):
    return f"{kib / 1024:.1f}" if kib is not None else "—"


def median(values):
    return statistics.median(values) if values else None


def ordered(items):
    return list(dict.fromkeys(items))


def report(results_dir, sample="loaded", breakdown_page=None):
    runs = load(results_dir)
    with open(os.path.join(results_dir, "meta.json")) as f:
        meta = json.load(f)
    configs = ordered(r["config"] for r in runs)
    pages = ordered(r["page"] for r in runs)

    def cell(config, page, getter):
        values = []
        for r in runs:
            if r["config"] == config and r["page"] == page and not r["error"]:
                s = r["samples"].get(sample)
                v = getter(s, r) if s else None
                if v is not None:
                    values.append(v)
        return median(values), len(values)

    lines = [f"# Phase 0 results: `{os.path.basename(os.path.normpath(results_dir))}`", ""]
    chromium = meta.get("chromium", {})
    host = meta["host"]
    lines += [
        f"- Snapshot builds: Chromium r{chromium.get('chromium_revision', '?')} "
        f"({chromium.get('got_revision', '?')[:12]}), display `{meta['display']}`, "
        f"memory cap `{meta.get('memory_max') or 'none'}`, settle {meta['settle']} s, "
        f"profile on `{meta.get('profile_fs', '?')}`, "
        f"{meta['repeats']} repeat(s)",
    ]
    for name, built in meta.get("builds", {}).items():
        lines.append(f"- `{name}` built from: {built}")
    lines += [
        f"- Host: {host.get('cpu')} x{host.get('cpus')}, "
        f"{host.get('MemTotal_kB', 0) // 1024} MB RAM, kernel {host['kernel']}",
        f"- Sample: `{sample}`. Values are medians in MB; `n` = successful runs.",
        "",
    ]

    def table(title, getter, note=None, fmt=mb):
        lines.append(f"## {title}")
        lines.append("")
        if note:
            lines.extend([note, ""])
        lines.append("| page | " + " | ".join(configs) + " |")
        lines.append("|---|" + "---:|" * len(configs))
        for page in pages:
            cells = []
            for c in configs:
                v, n = cell(c, page, getter)
                cells.append(fmt(v) + (f" (n={n})" if n != meta["repeats"] else ""))
            lines.append(f"| {page} | " + " | ".join(cells) + " |")
        lines.append("")

    table("Total PSS", lambda s, r: s["totals"]["Pss"],
          "Sum of PSS over all browser processes: the real footprint.")
    table("Unreclaimable PSS (anon + shmem)",
          lambda s, r: s["totals"].get("Pss_Anon", 0) + s["totals"].get("Pss_Shmem", 0),
          "What the kernel cannot drop and re-read from disk. File-backed pages "
          "(mostly the binary) can be evicted, at the price of re-reading them.")
    table("Process count", lambda s, r: s["totals"]["processes"],
          fmt=lambda v: f"{v:g}" if v is not None else "—")
    if runs and "cgroup" in (runs[0]["samples"].get(sample) or {}):
        table("cgroup peak", lambda s, r: (r.get("cgroup_final") or {}).get("peak"),
              "Highest cgroup charge during the run. Counts page cache only for files "
              "first read inside this run, so it understates the binary.")

    if any(r.get("perf") for r in runs):
        num = lambda v: f"{v:.1f}" if v is not None else "—"
        table("Load time (s)", lambda s, r: r.get("load_seconds"),
              "Navigation start to document complete.", fmt=num)
        table("Scroll: frames per second",
              lambda s, r: ((r.get("perf") or {}).get("scroll") or {}).get("fps"),
              "A 3 s smooth scroll driven by requestAnimationFrame (--perf). "
              "Meaningful on a real display.", fmt=num)
        table("Scroll: janky frames (%)",
              lambda s, r: ((r.get("perf") or {}).get("scroll") or {}).get("jank_percent"),
              "Frames over 25 ms (at least one missed 60 Hz refresh).", fmt=num)
        table("Video: dropped frames (%)",
              lambda s, r: ((r.get("perf") or {}).get("video") or {}).get("dropped_percent"),
              "10 s of the page's first video (720p on `video`), the browser's own "
              "counters.", fmt=num)

    breakdown_page = breakdown_page or (
        "wikipedia" if "wikipedia" in pages else (pages[0] if pages else None))
    if breakdown_page:
        kinds = ordered(
            p["kind"] for r in runs if r["page"] == breakdown_page and not r["error"]
            for p in (r["samples"].get(sample) or {}).get("processes", []))
        lines += [f"## PSS by process type: `{breakdown_page}`", ""]
        lines.append("| process | " + " | ".join(configs) + " |")
        lines.append("|---|" + "---:|" * len(configs))
        for kind in kinds:
            def getter(s, r, kind=kind):
                vals = [p["Pss"] for p in s["processes"] if p["kind"] == kind]
                return sum(vals) if vals else None
            lines.append(f"| {kind} | " + " | ".join(
                mb(cell(c, breakdown_page, getter)[0]) for c in configs) + " |")
        lines.append("")

    def site(url):
        host = url.split("://", 1)[-1].split("/", 1)[0]
        return ".".join(host.split(".")[-2:])

    elsewhere = [r for r in runs if r.get("final_url") and r["url"].startswith("http")
                 and site(r["final_url"]) != site(r["url"])]
    if elsewhere:
        lines += ["## Pages that ended up on another site", "",
                  "Not a measurement of the page asked for (e.g. Google's "
                  "\"unusual traffic\" interstitial after heavy automated use):", ""]
        for r in elsewhere:
            lines.append(f"- `{r['config']}` / `{r['page']}` #{r.get('repeat')}: "
                         f"{r['final_url'][:100]}")
        lines.append("")

    errors = [r for r in runs if r["error"]]
    if errors:
        lines += ["## Errors", ""]
        for r in errors:
            oom = (r.get("cgroup_final") or {}).get("oom_kill")
            lines.append(f"- `{r['config']}` / `{r['page']}` #{r.get('repeat')}: {r['error']}"
                         + (f" (oom_kill={oom})" if oom else ""))
        lines.append("")

    text = "\n".join(lines)
    with open(os.path.join(results_dir, f"report-{sample}.md"), "w") as f:
        f.write(text)
    return text
