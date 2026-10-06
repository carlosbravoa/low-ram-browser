"""Where an instance's memory goes, by Chromium component (memory-infra).

  python3 -m lrb_harness breakdown --binary PATH [--url URL] [--settle S]

Starts one lrb instance with the lean site-window flags, loads URL (default
about:blank), waits, and takes a detailed memory-infra dump over DevTools
tracing. Prints each top-level allocator (malloc, partition_alloc, v8,
blink_gc, skia, font_caches, ...) and selected children next to the
process's smaps totals. In single-process mode everything is one process.
"""

import json
import os
import shutil
import tempfile
import time

from . import cdp as cdplib
from . import procmem
from . import run as runlib
from .session import LEAN_ARGS


def _bytes(attrs, key):
    value = attrs.get(key, {}).get("value")
    if value is None:
        return 0
    return int(value, 16) if isinstance(value, str) else int(value)


def take_dump(client):
    client.keep_events = True
    client.events = []
    client.call("Tracing.start", {
        "traceConfig": {
            "includedCategories": ["disabled-by-default-memory-infra"],
            "excludedCategories": ["*"],
            "memoryDumpConfig": {"triggers": []},
        },
        "transferMode": "ReportEvents",
    })
    result = client.call("Tracing.requestMemoryDump",
                         {"deterministic": True, "levelOfDetail": "detailed"})
    client.call("Tracing.end")
    client.wait_event("Tracing.tracingComplete")
    client.keep_events = False
    trace = []
    for event in client.events:
        if event["method"] == "Tracing.dataCollected":
            trace += event["params"]["value"]
    if not result.get("success"):
        raise RuntimeError("memory dump failed")
    return trace


def allocators(trace):
    """{pid: {dump name: {"size": bytes, "effective": bytes}}} from the dump."""
    out = {}
    for event in trace:
        if event.get("ph") != "v" or "dumps" not in event.get("args", {}):
            continue
        dumps = event["args"]["dumps"]
        table = out.setdefault(event["pid"], {})
        for name, dump in dumps.get("allocators", {}).items():
            attrs = dump.get("attrs", {})
            table[name] = {"size": _bytes(attrs, "size"),
                           "effective": _bytes(attrs, "effective_size")}
        totals = dumps.get("process_totals", {})
        if totals:
            table["(process_totals)"] = {
                "size": _bytes({"v": totals.get("private_footprint_bytes") and
                                {"value": totals.get("private_footprint_bytes")} or {}}, "v"),
                "effective": 0}
    return out


def _collapse(table):
    """Folds per-region children with hashed names (shared_memory/<hex>,
    global/<hex>) into one line each."""
    out = {}
    for name, value in table.items():
        parent, _, leaf = name.rpartition("/")
        if parent and len(leaf) >= 12 and all(c in "0123456789abcdefABCDEF" for c in leaf):
            key = f"{parent}/<{'' if False else 'regions'}>"
            entry = out.setdefault(key, {"size": 0, "effective": 0, "count": 0})
            entry["size"] += value["size"]
            entry["count"] += 1
        else:
            out[name] = value
    return out


def diff(base, other, depth):
    """Per-dump growth from `base` to `other` (both raw tables)."""
    base, other = _collapse(base), _collapse(other)
    rows = []
    for name in set(base) | set(other):
        if name.count("/") >= depth:
            continue
        delta = other.get(name, {}).get("size", 0) - base.get(name, {}).get("size", 0)
        if abs(delta) >= 256 * 1024:
            rows.append((delta, name))
    rows.sort(reverse=True)
    return "\n".join(f"{d / 2**20:+8.2f}  {n}" for d, n in rows)


# Frames that are allocator machinery, not the code that wanted the memory.
_ALLOCATOR_FRAMES = ("malloc", "calloc", "realloc", "operator new", "partition_alloc",
                     "allocator_shim", "base::allocator", "ShimMalloc", "ShimCalloc",
                     "ShimRealloc", "PoissonAllocationSampler", "base::internal::",
                     "WTF::Partitions", "WTF::VectorBuffer", "WTF::Vector",
                     "std::__", "PartitionAlloc", "Allocate", "std::Cr::")


def heap_report(profile, top=30, frames=3):
    """Live sampled allocations grouped by their first frames outside the
    allocator. Sizes are estimates of live bytes (sampling)."""
    groups = {}
    for sample in profile.get("samples", []):
        stack = [f for f in sample.get("stack", []) if f]
        interesting = [f for f in stack if not any(a in f for a in _ALLOCATOR_FRAMES)]
        key = " <- ".join(f[:70] for f in (interesting or stack)[:frames]) or "(no stack)"
        groups[key] = groups.get(key, 0) + sample.get("total", sample.get("size", 0))
    total = sum(groups.values())
    lines = [f"Sampled live native allocations: {total / 2**20:.1f} MB", ""]
    for key, size in sorted(groups.items(), key=lambda kv: -kv[1])[:top]:
        lines.append(f"{size / 2**20:7.2f} MB  {key}")
    return "\n".join(lines)


def report(table, smaps, depth):
    table = _collapse(table)
    names = sorted(table, key=lambda n: -table[n]["size"])
    lines = [f"smaps: Pss_Anon {smaps.get('Pss_Anon', 0) / 1024:.1f} MB, "
             f"Pss_Shmem {smaps.get('Pss_Shmem', 0) / 1024:.1f} MB, "
             f"Pss_File {smaps.get('Pss_File', 0) / 1024:.1f} MB", "",
             f"{'MB':>8}  allocator dump"]
    for name in names:
        if name.count("/") >= depth or table[name]["size"] < 64 * 1024:
            continue
        count = table[name].get("count")
        lines.append(f"{table[name]['size'] / 2**20:8.2f}  {name}"
                     + (f" ({count} regions)" if count else ""))
    return "\n".join(lines)


def run(opts):
    os.makedirs(opts.profile_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="breakdown-", dir=opts.profile_dir)
    log = os.path.join(work, "lrb.log")
    argv = [os.path.abspath(opts.binary), f"--user-data-dir={work}/profile"] + \
        LEAN_ARGS + list(opts.extra_args) + ["about:blank"]
    proc = runlib.launch(argv, log, None, use_cgroup=False)
    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        if opts.heap_profile:
            client.call("Memory.startSampling",
                        {"samplingInterval": 16384, "suppressRandomness": True}, session)
        if opts.url != "about:blank":
            client.call("Page.navigate", {"url": opts.url}, session)
            runlib.wait_ready(client, session, opts.url, 30)
            try:
                client.evaluate(session, runlib.SCROLL_JS, await_promise=True)
            except cdplib.CDPError:
                pass
        time.sleep(opts.settle)
        if opts.discard:
            # As lrb's discard does: about:blank, then critical pressure at 1 s
            # and 11 s (ActivityTracker, LrbContentBrowserClient).
            client.call("Page.navigate", {"url": "about:blank"}, session)
            time.sleep(1)
            client.call("Memory.simulatePressureNotification", {"level": "critical"}, session)
            time.sleep(10)
            client.call("Memory.simulatePressureNotification", {"level": "critical"}, session)
            time.sleep(5)
        if opts.pressure:
            client.call("HeapProfiler.collectGarbage", session_id=session)
            client.call("Memory.simulatePressureNotification", {"level": "critical"}, session)
            time.sleep(3)
        smaps = procmem.smaps_rollup(proc.pid) or {}
        heap = None
        if opts.heap_profile:
            heap = client.call("Memory.getSamplingProfile", {}, session)["profile"]
            client.call("Memory.stopSampling", {}, session)
        trace = take_dump(client)
        client.close()
        tables = allocators(trace)
        table = tables.get(proc.pid) or (max(tables.values(), key=len) if tables else {})
        text = report(table, smaps, opts.depth)
        if heap is not None:
            text += "\n\n" + heap_report(heap)
        if opts.json:
            with open(opts.json, "w") as f:
                json.dump({"url": opts.url, "smaps": smaps, "allocators": table}, f, indent=1)
        if opts.compare:
            with open(opts.compare) as f:
                base = json.load(f)
            text += (f"\n\nChange from {opts.compare} (unreclaimable "
                     f"{(base['smaps']['Pss_Anon'] + base['smaps']['Pss_Shmem']) / 1024:.1f} -> "
                     f"{(smaps.get('Pss_Anon', 0) + smaps.get('Pss_Shmem', 0)) / 1024:.1f} MB):\n"
                     + diff(base["allocators"], table, opts.depth))
        return text
    finally:
        runlib.stop(proc, None)
        shutil.rmtree(work, ignore_errors=True)
