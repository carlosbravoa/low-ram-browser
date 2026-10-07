"""Phase 0 memory harness.

  python3 -m lrb_harness list
  python3 -m lrb_harness run [--configs a,b] [--pages a,b] [--repeats N] ...
  python3 -m lrb_harness report RESULTS_DIR [--sample loaded|pressured|startup]
  python3 -m lrb_harness multi --configs lrb-single,lrb-default:tabs --counts 1,3,5 ...
  python3 -m lrb_harness multi-report RESULTS_DIR

Run from the phase0/ directory.
"""

import argparse
import os
import shlex

from . import breakdown, configs, multi, report, run, session


def main():
    root = os.path.dirname(run.PHASE0)
    with open(os.path.join(root, "CHROMIUM_REVISION")) as f:
        revision = f.read().strip()

    parser = argparse.ArgumentParser(prog="lrb_harness")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="show configurations")

    p = sub.add_parser("run", help="measure configurations x pages")
    p.add_argument("--configs", help="comma-separated (default: %s)" % ",".join(configs.DEFAULT_SET))
    p.add_argument("--pages", help="comma-separated page names (default: all)")
    p.add_argument("--pages-file", default=os.path.join(run.PHASE0, "pages.txt"))
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--settle", type=float, default=10, help="seconds after scrolling before sampling")
    p.add_argument("--perf", action="store_true",
                   help="also measure smoothness: a 3 s smooth scroll's frame times, and 10 s "
                        "of the page's first video (dropped frames); use --display native")
    p.add_argument("--load-timeout", type=float, default=30)
    p.add_argument("--display", choices=["headless", "native"], default="headless",
                   help="headless: Ozone headless platform (no window, reproducible); "
                        "native: the real X11/Wayland display")
    p.add_argument("--memory-max", help="cgroup cap, e.g. 512M; runs that exceed it are OOM-killed")
    p.add_argument("--no-cgroup", dest="cgroup", action="store_false",
                   help="account by process tree instead (for hosts without systemd)")
    p.add_argument("--prebuilt", default=os.path.join(root, "prebuilt", revision))
    p.add_argument("--binary", action="append", default=[],
                   help="override a binary, e.g. content_shell=/path/to/out/lrb/content_shell")
    p.add_argument("--extra-args", type=shlex.split, default=[],
                   help="extra browser flags appended to every run (quoted string)")
    p.add_argument("--profile-dir", default=os.path.join(root, ".profiles"),
                   help="where per-run profiles go; use a tmpfs path to measure "
                        "a RAM-backed profile on purpose")
    p.add_argument("--out", help="results directory (default: results/raw/<timestamp>)")

    p = sub.add_parser("multi", help="cost of K sites: K instances, or K windows in one browser")
    p.add_argument("--configs", required=True,
                   help="comma-separated; 'name' = one instance per page, 'name:tabs' = one browser")
    p.add_argument("--counts", default="1,2,3,4,5", help="numbers of sites open at once")
    p.add_argument("--pages", help="comma-separated, in the order sites are added")
    p.add_argument("--pages-file", default=os.path.join(run.PHASE0, "pages.txt"))
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--settle", type=float, default=10)
    p.add_argument("--load-timeout", type=float, default=30)
    p.add_argument("--display", choices=["headless", "native"], default="headless")
    p.add_argument("--no-cgroup", dest="cgroup", action="store_false")
    p.add_argument("--prebuilt", default=os.path.join(root, "prebuilt", revision))
    p.add_argument("--binary", action="append", default=[])
    p.add_argument("--extra-args", type=shlex.split, default=[])
    p.add_argument("--profile-dir", default=os.path.join(root, ".profiles"))
    p.add_argument("--out")

    p = sub.add_parser("session", help="a K-site session through lrb_coordinator under a cap")
    p.add_argument("--lrb-dir", required=True, help="directory with lrb and lrb_coordinator")
    p.add_argument("--pages", default="wikipedia,github,reddit,amazon,bbc")
    p.add_argument("--pages-file", default=os.path.join(run.PHASE0, "pages.txt"))
    p.add_argument("--variants", default="discard,nodiscard")
    p.add_argument("--memory-max")
    p.add_argument("--repeats", type=int, default=2)
    p.add_argument("--settle", type=float, default=5)
    p.add_argument("--load-timeout", type=float, default=30)
    p.add_argument("--profile-dir", default=os.path.join(root, ".profiles"))
    p.add_argument("--out")

    p = sub.add_parser("breakdown", help="memory by Chromium component for one lrb instance")
    p.add_argument("--binary", required=True, help="path to lrb")
    p.add_argument("--url", default="about:blank")
    p.add_argument("--settle", type=float, default=5)
    p.add_argument("--pressure", action="store_true",
                   help="GC + critical pressure before the dump")
    p.add_argument("--depth", type=int, default=2, help="show dump names up to this many '/'")
    p.add_argument("--extra-args", type=shlex.split, default=[])
    p.add_argument("--json", help="also write the full table here")
    p.add_argument("--compare", help="a --json from another run: print the change")
    p.add_argument("--heap-profile", action="store_true",
                   help="sample native allocations from launch; list what's live, by caller")
    p.add_argument("--discard", action="store_true",
                   help="after loading, discard as lrb does (about:blank + pressure)")
    p.add_argument("--profile-dir", default=os.path.join(root, ".profiles"))

    p = sub.add_parser("session-report", help="summarize a session results directory")
    p.add_argument("results_dir")

    p = sub.add_parser("multi-report", help="summarize a multi results directory")
    p.add_argument("results_dir")

    p = sub.add_parser("report", help="summarize a results directory")
    p.add_argument("results_dir")
    p.add_argument("--sample", default="loaded", choices=["startup", "loaded", "pressured"])
    p.add_argument("--breakdown-page")

    opts = parser.parse_args()
    if opts.command == "list":
        for name, cfg in configs.CONFIGS.items():
            flags = cfg["args"] + [f"--disable-features={','.join(cfg['disable_features'])}"
                                   if cfg.get("disable_features") else ""]
            if cfg.get("js_flags"):
                flags.append("--js-flags=" + " ".join(cfg["js_flags"]))
            default = "*" if name in configs.DEFAULT_SET else " "
            print(f"{default} {name:24} {cfg['binary']:14} {' '.join(f for f in flags if f)}")
    elif opts.command == "run":
        out = run.run(opts)
        print(report.report(out))
    elif opts.command == "multi":
        print(multi.report(multi.run(opts)))
    elif opts.command == "session":
        print(session.report(session.run(opts)))
    elif opts.command == "breakdown":
        print(breakdown.run(opts))
    elif opts.command == "session-report":
        print(session.report(opts.results_dir))
    elif opts.command == "multi-report":
        print(multi.report(opts.results_dir))
    else:
        print(report.report(opts.results_dir, opts.sample, opts.breakdown_page))


if __name__ == "__main__":
    main()
