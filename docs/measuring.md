# Measuring

How lrb's numbers are taken (`phase0/lrb_harness`), so anyone can check
them. The harness started as "Phase 0", measuring stock Chromium binaries
before any build of our own, hence its directory name.

## Method

Each run:

1. Fresh profile on real disk (`.profiles/`, not tmpfs).
2. Launch in its own systemd user scope (cgroup v2), optionally capped with
   `--memory-max` (swap disabled for the scope, so overflow means OOM kill).
   The browser gets `DBUS_SESSION_BUS_ADDRESS=disabled:` so it can't move
   itself to another cgroup.
3. Connect over the DevTools protocol, wait 2 s, sample (`startup`).
4. Navigate, wait for `readyState == complete` (30 s max), scroll the page a
   screen at a time and back, wait `--settle` seconds, sample (`loaded`).
5. Force garbage collection and a critical memory-pressure signal, wait 3 s,
   sample (`pressured`).
6. Kill the cgroup.

Configurations are interleaved per page, so drift in live sites hits all of
them alike. Default: 3 repeats, medians reported.

## Metrics

All from `/proc/<pid>/smaps_rollup`, summed over every process in the cgroup.

- **Total PSS.** Proportional set size: shared pages are split between the
  processes that map them, so the sum is the real footprint.
- **Unreclaimable PSS (anon + shmem).** What the kernel can't drop and
  re-read from disk. **The primary metric**: on a small device this is what
  decides whether it runs.
- **File PSS** (total minus unreclaimable) is mostly the binary's code.
  The kernel can evict it, but the browser then re-reads it from storage
  constantly, so a smaller binary still matters (the official build).
- **cgroup peak.** Highest charge during the run. It counts page cache only
  for files first read inside that run, so it understates the binary.
- Per process type (browser, renderer, gpu-process, utility:network, ...).
- From the page: V8 heap (`Runtime.getHeapUsage`), DOM counters.

## Configurations

`python3 -m lrb_harness list` prints the exact flags.

| Name | What it tests |
|---|---|
| `cs-default` | content_shell as shipped: sandboxed, one process per site |
| `cs-fewproc` | Multi-process with the fewest processes: one renderer, GPU and network in the browser process. Isolated-tab cost = this minus `cs-single`. |
| `cs-single` | Everything in one process, no zygotes |
| `cs-single-lowmem` | + low-end device mode, no back/forward cache, V8 `--optimize-for-size` |
| `cs-single-lowmem-nogpu` | + software compositing |
| `cs-single-lite` | + V8 lite mode (jitless: **no WebAssembly**, slow); measures the ceiling only |
| `chrome-default`, `chrome-single-lowmem` | Full Chromium, the cost of the `//chrome` layer |
| `lrb-*` | Each `cs-*` config on our own build (`--binary lrb=<path>`) |

## Caveats

- Snapshot builds are not official builds: no PGO, and they include debug
  symbols. Absolute file-backed numbers will drop in the official build.
- Snapshot builds have no proprietary codecs: sites serving only H.264
  fall back or show no video. This understates such sites a lot: on CNN
  our H.264 build plays four autoplaying loops worth 180-270 MB that the
  snapshot skips (the official build).
- Live sites change daily and serve different content by region and to
  headless browsers. Compare configurations within one sweep.
- `--display headless` (default) uses Ozone's headless platform: real layout,
  raster and compositing, no window. `--display native` uses the desktop.

## Running

```sh
cd phase0
python3 -m lrb_harness run                                      # default sweep, ~1.5 h
python3 -m lrb_harness run --configs cs-single --memory-max 256M # does it survive?
python3 -m lrb_harness run --binary content_shell=../dist/x/content-shell/content_shell
python3 -m lrb_harness report ../results/raw/<dir> --sample pressured
```
