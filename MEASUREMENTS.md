# Measurements

The numbers behind lrb's design, and how to reproduce each one. Method:
[docs/measuring.md](docs/measuring.md).

**Metric:** unreclaimable PSS (anonymous + shared memory), summed over all
of a browser's processes. This is what the kernel can't drop and re-read
from disk; the binary's file-backed pages are shared and evictable.
Medians of 3 runs unless noted.

**Host:** AMD Ryzen 7 5700X, 62 GB, Linux 7.0, headless (Ozone headless),
profiles on ext4. Live sites change daily, so compare configurations
within one sweep (the harness interleaves them), not across dates.

Setup for every command below: build lrb (docs/building.md), unpack it to
`dist/x64`, `phase0/fetch_prebuilt.sh` for the Chromium snapshot
references, then `cd phase0`.

## One lrb instance, by page

Chromium Stable 154, lrb's defaults (`lrb-lean-pressure`):

| page | MB |
|---|---:|
| blank | 30.1 |
| static article | 42.2 |
| Wikipedia | 85.7 |
| GitHub | 129.4 |
| BBC News | 188.1 |
| Amazon | 174.3 |
| Maps | 249.5 |
| YouTube | 272.1 |
| CNN | 275.0 |

```sh
python3 -m lrb_harness run --binary lrb=../dist/x64/content-shell/content_shell \
    --configs lrb-lean-pressure
```

## Site per window against Chrome's model

Five sites at once: one lrb instance per site against one browser with five
windows, each site in its own renderer, as Chrome does it.

| | unreclaimable | total PSS |
|---|---:|---:|
| lrb, one instance per site | 741 MB | 909 MB |
| Chrome's model | 1083 MB | 1290 MB |
| difference | −32% | −30% |

Putting all five sites in one shared process would cost 634 MB: keeping
sites apart costs about 25-50 MB per extra window.

```sh
python3 -m lrb_harness multi --binary lrb=../dist/x64/content-shell/content_shell \
    --configs lrb-single-lowmem-nogpu,lrb-single-lowmem-nogpu:tabs,lrb-default:tabs \
    --counts 1,2,3,5
```

## The //chrome layer

The Chrome product layer costs about 70 MB of anonymous memory over
content_shell for the same page (Chromium snapshot `chrome-default` against
`cs-default`). `python3 -m lrb_harness run` with the default configs.

## Defaults, each measured

| Choice | Effect | Configs to compare |
|---|---|---|
| Low-end-device mode + software compositing | About one memory-cap step: every site works under a 512 MB cap (plain single-process loses CNN and YouTube), 7 of 10 at 256 MB | `lrb-single`, `lrb-single-lowmem-nogpu`, with `--memory-max 512M` / `256M` |
| Memory pressure signal (`patches/0001`) | Frees 15-40% (30-170 MB) on real sites; moves Maps and Amazon from dying to surviving at 256 MB | `lrb-lean-nopressure`, `lrb-lean-pressure` with `--memory-max 256M` |
| No autoplay before interaction (`patches/0002`) | CNN's front page: no video plays on arrival (0 of 6, against 5 of 6), 45% less memory | `lrb-lean-autoplay`, `lrb-lean-pressure` --pages cnn |
| Without Chromium's field trial testing config | 1.4 MB less on a blank page, 4.9 on Wikipedia, 17 on GitHub, 22 on CNN | `lrb-lean-trials`, `lrb-lean-pressure` |
| Content blocking | BBC News −42% (ad auctions and trackers); little on pages with few third-party ads. The engine costs 0.8 MB per instance (shared read-only file) | `lrb-lean-pressure`, `lrb-lean-adblock` (needs `dist/adblock/full.adb`: `lrb --lrb-update-lists=dist/adblock/full.adb`) |
| Software compositing against the GPU | A window: 48 MB against 60 MB (one NVIDIA machine; varies by GPU and driver) | `lrb-lean-pressure` with and without `--extra-args=--lrb-gpu` on the real display |
| Native bar against an HTML bar | About 2 MB per window against 6.5 MB | `--content-shell-hide-toolbar` against the default, `--display native` |
| Discarding a background window | Frees 81 of a page's 96 MB (84%), keeping the window and its history; Chromium's own `WebContents::Discard()` kept 33 MB more | `python3 -m lrb_harness session --lrb-dir ../dist/x64/content-shell` |
| Build configuration (`build/args.gn`) | About 7 MB per browser, almost all file-backed | `cs-*` (snapshot) against `lrb-*` (our build) |

## Raspberry Pi 3 Model B+ (1 GB)

`tools/device_test.sh` on the v0.1.0-alpha.1 arm64 build: Debian 13 64-bit,
X11, 905 MB RAM with 905 MB zram. One run each.

| page | software | GPU |
|---|---:|---:|
| blank | 32.3 | 39.3 |
| Wikipedia | 85.8 | 93.4 |
| BBC News | 154.7 | 161.1 |
| GitHub | 104.6 | 124.6 |
| Reddit | 123.2 | 133.9 |
| YouTube | 253.6 | 308.6 |

Five sites opened one after another through `lrb_coordinator` (content
blocking on): memory pressure began at the third site, the coordinator
discarded the least recently used windows, and five open sites cost 325 MB
with 242 MB still free. No kernel OOM kill.

```sh
tools/device_test.sh content-shell     # on the device, from its desktop
```

## Chromium releases

Stable 154 against the main-branch commit it replaced (157.0.8086.0): the
same overall. Blank and static pages within 0.5 MB; GitHub 25 MB cheaper,
CNN 12 MB dearer. `build/update_stable.sh` repeats this comparison for
every release and fails on a page whose runs are all over 5% dearer.

```sh
python3 -m lrb_harness run --configs lrb-lean-pressure,base-lean-pressure \
    --binary lrb=../dist/x64/content-shell/content_shell \
    --binary lrb-base=<previous build>/content-shell/content_shell
```

## Caveats

- YouTube and other Google pages may redirect to google.com/sorry after
  many automated loads from one IP; the report lists runs that ended on
  another site. Don't trust those numbers when it does.
- Chromium snapshot builds can't decode H.264, so video sites look cheaper
  on them; measure video on lrb's own build.
- A profile on tmpfs (Ubuntu's `/tmp`) turns disk caches into RAM; the
  harness puts profiles in `.profiles/` on disk.
