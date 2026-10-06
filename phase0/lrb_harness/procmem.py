"""Memory accounting from /proc and cgroup v2.

Primary metric: sum of PSS over every process of the browser. PSS splits
shared pages (the binary, ICU data, V8 snapshot) fairly between processes, so
the sum is the real footprint of the set rather than the inflated RSS sum.
"""

import os

SMAPS_FIELDS = (
    "Rss", "Pss", "Pss_Anon", "Pss_File", "Pss_Shmem",
    "Private_Clean", "Private_Dirty", "Swap", "SwapPss",
)

CGROUP_STAT_FIELDS = ("anon", "file", "kernel", "shmem", "sock", "file_mapped")


def cgroup_dir(pid):
    with open(f"/proc/{pid}/cgroup") as f:
        for line in f:
            if line.startswith("0::"):
                return "/sys/fs/cgroup" + line[3:].strip()
    return None


def cgroup_pids(path):
    with open(os.path.join(path, "cgroup.procs")) as f:
        return [int(p) for p in f.read().split()]


def tree_pids(root):
    """Fallback without cgroups: root plus all descendants."""
    children = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/stat") as f:
                stat = f.read()
        except OSError:
            continue
        ppid = int(stat.rsplit(")", 1)[1].split()[1])
        children.setdefault(ppid, []).append(int(entry))
    out, stack = [], [root]
    while stack:
        pid = stack.pop()
        out.append(pid)
        stack.extend(children.get(pid, []))
    return out


def smaps_rollup(pid):
    """Values in KiB; None if the process went away."""
    values = {}
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            for line in f:
                parts = line.split()
                key = parts[0].rstrip(":")
                if key in SMAPS_FIELDS:
                    values[key] = int(parts[1])
    except OSError:
        return None
    return values


def process_kind(pid):
    """Label a Chromium process from its command line."""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            argv = [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return "gone"
    if len(argv) == 1:
        # Chromium rewrites child command lines as one space-joined string.
        argv = argv[0].split()
    if not argv:
        return "unknown"
    exe = os.path.basename(argv[0])
    if "crashpad" in exe:
        return "crashpad"
    if exe in ("systemd-run", "env"):
        return "launcher"
    kind, sub = None, None
    for a in argv[1:]:
        if a.startswith("--type="):
            kind = a.split("=", 1)[1]
        elif a.startswith("--utility-sub-type="):
            sub = a.split("=", 1)[1].split(".")[0]
    if kind is None:
        return "browser"
    if kind == "zygote" and "--no-zygote-sandbox" in argv:
        return "zygote:unsandboxed"
    if kind == "utility" and sub:
        return f"utility:{sub}"
    return kind


def cgroup_stats(path):
    out = {}
    for name in ("memory.current", "memory.peak"):
        try:
            with open(os.path.join(path, name)) as f:
                out[name.split(".")[1]] = int(f.read()) // 1024
        except OSError:
            pass
    try:
        with open(os.path.join(path, "memory.stat")) as f:
            for line in f:
                key, value = line.split()
                if key in CGROUP_STAT_FIELDS:
                    out[key] = int(value) // 1024
    except OSError:
        pass
    try:
        with open(os.path.join(path, "memory.events")) as f:
            for line in f:
                key, value = line.split()
                if key == "oom_kill":
                    out["oom_kill"] = int(value)
    except OSError:
        pass
    return out


def snapshot(root_pid, cgroup=None):
    """One sample: per-process smaps_rollup plus totals (all KiB)."""
    pids = cgroup_pids(cgroup) if cgroup else tree_pids(root_pid)
    processes = []
    for pid in pids:
        mem = smaps_rollup(pid)
        if mem is None:
            continue
        kind = process_kind(pid)
        if kind == "launcher":
            continue
        processes.append({"pid": pid, "kind": kind, **mem})
    totals = {k: sum(p.get(k, 0) for p in processes) for k in SMAPS_FIELDS}
    totals["processes"] = len(processes)
    sample = {"totals": totals, "processes": processes}
    if cgroup:
        sample["cgroup"] = cgroup_stats(cgroup)
    return sample
