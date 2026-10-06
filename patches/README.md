# Patches

`git am`-style patches against `CHROMIUM_COMMIT`, applied in name order by
`build/apply_patches.sh`. Number them `0001-...patch`. Keep each one small and
focused so rebasing onto a new Chromium commit stays cheap.

Developing a patch: work on the `lrb` branch of the checkout
(`build/apply_patches.sh` recreates it from `patches/`), build and test in
`out/dev-x64` (`build/build.sh x64 dev`), then export with
`git format-patch -1 -o patches/ --start-number N`. Measure in the official
build. `dist/x64/content-shell/COMMIT` records which patch commit a packaged
build came from.

## Applied

| Patch | What | Evidence |
|---|---|---|
| 0001 memory_pressure: Linux evaluator | `//content` had no memory pressure source on Linux. Polls cgroup v2 budgets (memory.max/high vs current − inactive_file) up to the root plus MemAvailable, and votes moderate below 20% and critical below 10% of the tightest. Feature `LinuxMemoryPressureEvaluator` (on by default), params `moderate_percent`, `critical_percent`, `poll_period`, `psi_moderate_avg10`. Log with `--vmodule=system_memory_pressure_evaluator_linux=1`. | MEASUREMENTS.md |

| 0002 blink: MutedAutoplayRequiresUserActivation | Runtime feature (off by default) removing muted video's autoplay exemption: it waits for user activation like audible media. lrb enables it with `--autoplay-policy=document-user-activation-required`. | MEASUREMENTS.md |

## Retired

0003-0006 changed content_shell for lrb (camera/microphone requests, the
"leave this page?" answer, opening URLs and activation for tabs, find
results). Since 2026-10-05 lrb has its own copy of the content_shell code it
uses (`lrb/browser/shell.cc` and the clients), where these are plain code.

## Planned

- No systemd scope registration over D-Bus (`components/dbus/xdg/systemd.cc`).
- Cap or disable disk caches when the profile is on a RAM-backed filesystem.
