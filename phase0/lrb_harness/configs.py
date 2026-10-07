"""Browser configurations under test.

Every name below was checked against the pinned Chromium commit
(CHROMIUM_COMMIT); feature and switch names drift between releases, so
re-check them when the pin moves.
"""

import os

WINDOW = "1280,800"

# Services that normally get their own process. Pulling them into the browser
# process is the multi-process way of saving memory.
IN_PROCESS_SERVICES = [
    "--in-process-gpu",
    "--enable-features=NetworkServiceInProcess2",
]
DISABLED_FEATURES_FEWPROC = ["AudioServiceOutOfProcess"]

# Runtime knobs that cut memory without code changes.
# BackForwardCache keeps the previous page alive in memory after navigating.
LOW_MEMORY = ["--enable-low-end-device-mode"]
DISABLED_FEATURES_LOWMEM = ["BackForwardCache"]

# The zygotes only exist to fork sandboxed children; a single-process browser
# never forks, so they are pure overhead there (~45 MB PSS measured).
SINGLE = ["--single-process", "--no-zygote", "--no-sandbox"]

# V8 options: optimize_for_size keeps the optimizing JIT but favours smaller
# code and heaps. lite_mode implies jitless, which also disables WebAssembly,
# so it breaks some mainstream sites; measured only to know its ceiling.
JS_SIZE = "--optimize-for-size"
JS_LITE = "--lite-mode"

CONFIGS = {
    # Baseline: content_shell as shipped (sandboxed, process per site).
    "cs-default": {"binary": "content_shell", "args": []},
    # Multi-process, as few processes as possible: one renderer for all sites,
    # GPU and network inside the browser process.
    "cs-fewproc": {
        "binary": "content_shell",
        "args": IN_PROCESS_SERVICES + [
            "--renderer-process-limit=1",
            "--disable-site-isolation-trials",
        ],
        "disable_features": DISABLED_FEATURES_FEWPROC,
    },
    # Everything in one process.
    "cs-single": {
        "binary": "content_shell",
        "args": SINGLE + IN_PROCESS_SERVICES,
        "disable_features": DISABLED_FEATURES_FEWPROC,
    },
    "cs-single-lowmem": {
        "binary": "content_shell",
        "args": SINGLE + IN_PROCESS_SERVICES + LOW_MEMORY,
        "disable_features": DISABLED_FEATURES_FEWPROC + DISABLED_FEATURES_LOWMEM,
        "js_flags": [JS_SIZE],
    },
    # Software compositing: no GPU thread or GPU memory at all.
    "cs-single-lowmem-nogpu": {
        "binary": "content_shell",
        "args": SINGLE + IN_PROCESS_SERVICES + LOW_MEMORY + ["--disable-gpu"],
        "disable_features": DISABLED_FEATURES_FEWPROC + DISABLED_FEATURES_LOWMEM,
        "js_flags": [JS_SIZE],
    },
    "cs-single-lite": {
        "binary": "content_shell",
        "args": SINGLE + IN_PROCESS_SERVICES + LOW_MEMORY,
        "disable_features": DISABLED_FEATURES_FEWPROC + DISABLED_FEATURES_LOWMEM,
        "js_flags": [JS_LITE],
    },
    # References for what the //chrome layer costs on top of //content.
    "chrome-default": {"binary": "chrome", "args": []},
    "chrome-single-lowmem": {
        "binary": "chrome",
        "args": SINGLE + IN_PROCESS_SERVICES + LOW_MEMORY,
        "disable_features": DISABLED_FEATURES_FEWPROC + DISABLED_FEATURES_LOWMEM,
        "js_flags": [JS_SIZE],
    },
}

# Our own content_shell build, under the same flags as each cs-* config:
# lrb-default, lrb-single, ... Point at it with --binary lrb=<path>. Running
# them in the same sweep as cs-* interleaves the two binaries, so live-site
# drift hits both alike.
for _name, _cfg in list(CONFIGS.items()):
    if _name.startswith("cs-"):
        CONFIGS["lrb-" + _name[3:]] = dict(_cfg, binary="lrb")

# Memory pressure evaluator (patches/0001) on our build, on top of the leanest
# config: off (behaviour without the patch), its defaults (250 ms poll,
# moderate below 30%, critical below 15%), and the first, slower settings
# that lost to the defaults in MEASUREMENTS.md.
# lrb's own defaults also leave out Chromium's field trial testing config,
# which content_shell applies (MEASUREMENTS.md).
_LEAN_WITH_TRIALS = CONFIGS["lrb-single-lowmem-nogpu"]
_LEAN = dict(_LEAN_WITH_TRIALS,
             args=_LEAN_WITH_TRIALS["args"] + ["--disable-field-trial-config"])
CONFIGS["lrb-lean-nopressure"] = dict(
    _LEAN, disable_features=_LEAN["disable_features"] + ["LinuxMemoryPressureEvaluator"])
CONFIGS["lrb-lean-pressure"] = dict(_LEAN)
CONFIGS["lrb-lean-pressure-slow"] = dict(
    _LEAN, args=_LEAN["args"] + [
        "--enable-features=LinuxMemoryPressureEvaluator:"
        "poll_period/1s/moderate_percent/20/critical_percent/10"])

# Media autoplay as before lrb's video policy (patches/0002 and lrb's
# defaults): muted video autoplays, everything autoplays without a gesture.
CONFIGS["lrb-lean-autoplay"] = dict(
    _LEAN, args=_LEAN["args"] + ["--autoplay-policy=no-user-gesture-required",
                                 "--disable-blink-features=MutedAutoplayRequiresUserActivation"])

# Content blocking (lrb/adblock): the lean instance plus an engine file. The
# file's path comes from $LRB_ADBLOCK_DIR (default dist/adblock):
# full.adb (all lists) and lean.adb (network rules of the main lists).
CONFIGS["lrb-lean-adblock"] = dict(
    _LEAN, args=_LEAN["args"] + ["--lrb-adblock-file={adblock_dir}/full.adb"])
CONFIGS["lrb-lean-adblock-lean"] = dict(
    _LEAN, args=_LEAN["args"] + ["--lrb-adblock-file={adblock_dir}/lean.adb"])

# The GPU against software compositing (the first-start question): the
# lean config with the GPU (no --disable-gpu). Smoothness needs --perf and
# --display native.
CONFIGS["lrb-lean-gpu"] = dict(
    _LEAN, args=[a for a in _LEAN["args"] if a != "--disable-gpu"])

# With Chromium's field trial testing config (the testing group of every
# experiment in testing/variations/fieldtrial_testing_config.json), as
# lrb-lean-* measured before 2026-10-05.
CONFIGS["lrb-lean-trials"] = dict(_LEAN_WITH_TRIALS)

# Another build of ours (an earlier Chromium commit, before a rebase) under
# the same flags: base-<x> for every lrb-<x>, with --binary lrb-base=<path>.
# Interleaved with lrb-* in one sweep, live-site drift hits both alike.
for _name, _cfg in list(CONFIGS.items()):
    if _name.startswith("lrb-"):
        CONFIGS["base-" + _name[4:]] = dict(_cfg, binary="lrb-base")

# Configurations run when none are named on the command line.
DEFAULT_SET = [
    "cs-default", "cs-fewproc", "cs-single", "cs-single-lowmem",
    "cs-single-lowmem-nogpu", "chrome-default", "chrome-single-lowmem",
]

# Flags that make runs quieter and reproducible without changing what we
# measure. Applied to every run of that binary.
COMMON = {
    "content_shell": [
        f"--content-shell-host-window-size={WINDOW.replace(',', 'x')}",
    ],
    "chrome": [
        f"--window-size={WINDOW}",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-background-networking",
        "--disable-component-update",
        "--disable-sync",
        "--password-store=basic",
    ],
}
COMMON["lrb"] = COMMON["content_shell"]
COMMON["lrb-base"] = COMMON["content_shell"]

# Where each binary lives inside a snapshot directory (prebuilt/<rev>/).
PREBUILT_LAYOUT = {
    "content_shell": "content-shell/content_shell",
    "chrome": "chrome-linux/chrome",
}


def command_line(name, binary_path, user_data_dir, display, extra_args=()):
    cfg = CONFIGS[name]
    args = [binary_path, f"--user-data-dir={user_data_dir}", "--remote-debugging-port=0"]
    args += COMMON[cfg["binary"]]
    if display == "headless":
        args.append("--ozone-platform=headless")
    # Chromium keeps only the last --enable-features, so merge them.
    enabled = []
    adblock_dir = os.path.abspath(os.environ.get("LRB_ADBLOCK_DIR") or os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "dist", "adblock"))
    for a in cfg["args"]:
        a = a.replace("{adblock_dir}", adblock_dir)
        if a.startswith("--enable-features="):
            enabled += a.split("=", 1)[1].split(",")
        else:
            args.append(a)
    if enabled:
        args.append("--enable-features=" + ",".join(enabled))
    if cfg.get("disable_features"):
        args.append("--disable-features=" + ",".join(cfg["disable_features"]))
    if cfg.get("js_flags"):
        args.append("--js-flags=" + " ".join(cfg["js_flags"]))
    args += list(extra_args)
    args.append("about:blank")
    return args
