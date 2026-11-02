# Patches

`git am`-style patches against `CHROMIUM_COMMIT`, applied in name order by
`build/apply_patches.sh`. Number them `0001-...patch`. Keep each one small and
focused so rebasing onto a new Chromium commit stays cheap.

Developing a patch: work on the `lrb` branch of the checkout
(`build/apply_patches.sh` recreates it from `patches/`), build and test in
`out/dev-x64` (`build/build.sh x64 dev`), then export with
`git format-patch -1 -o patches/ --start-number N`. `dist/x64/content-shell/COMMIT` records which patch commit a packaged
build came from.

## Applied

| Patch | What | Why |
|---|---|---|
| 0001 memory_pressure: Linux evaluator | `//content` had no memory pressure source on Linux. Polls cgroup v2 budgets (memory.max/high vs current − inactive_file) up to the root plus MemAvailable, and votes moderate below 20% and critical below 10% of the tightest. Feature `LinuxMemoryPressureEvaluator` (on by default), params `moderate_percent`, `critical_percent`, `poll_period`, `psi_moderate_avg10`. Log with `--vmodule=system_memory_pressure_evaluator_linux=1`. | Lets lrb put background tabs and windows to sleep before the kernel kills anything: under a 256 MB cap it kept 8 of 10 test sites alive, against 6 without |
| 0002 blink: MutedAutoplayRequiresUserActivation | Runtime feature (off by default) removing muted video's autoplay exemption: it waits for user activation like audible media. lrb enables it with `--autoplay-policy=document-user-activation-required`. | Muted autoplaying video costs memory and CPU on pages the user hasn't interacted with |
| 0003 content: discardable memory allocator in single-process mode | In single-process mode the browser left `base::DiscardableMemoryAllocator` unset until the in-process renderer started (crbug.com/503724). On a Wayland desktop with client-side decorations the window frame's blurred shadow rasters before that and crashed every window (null allocator in `SkDiscardableMemory::Create`). The browser now installs its manager; the renderer only when none is set. Upstreamable. | Crash on GNOME Wayland (0.1.0-alpha.4 and before with `--ozone-platform=wayland`) gone |
| 0004 memory_pressure: absolute caps on the Linux evaluator | Feature params `moderate_max_mb` (512) and `critical_max_mb` (256) on 0001's evaluator: a level engages below its percent of the limit or its cap, whichever is less (0: no cap; a percent over 100 still means always). The coordinator applies the same rule (`--moderate-max-mb`, `--critical-max-mb`). | On large machines a percentage alone counted gigabytes free as pressure (15% of 32 GB is 4.8 GB), so caches were purged for nothing; under 1.7 GB of limit the percentages stay the tighter ones |
