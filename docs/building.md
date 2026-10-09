# Building

## Requirements

- Linux x86-64 (Ubuntu LTS or Debian recommended by Chromium).
- **Disk:** about 100 GB free (shallow checkout ~35 GB, an official build
  directory ~30-50 GB). SSD strongly preferred.
- **RAM:** 32 GB or more. Official builds use ThinLTO, and linking is the
  memory peak. With less RAM, reduce parallelism: `autoninja -j 8`.
- **CPU:** a full build is several hours on 8-16 cores.

## Steps

```sh
git clone <this repository> low-ram-browser
cd low-ram-browser

build/setup_checkout.sh                         # depot_tools + Chromium at CHROMIUM_COMMIT
sudo ~/chromium/src/build/install-build-deps.sh # once per machine
build/apply_patches.sh                          # patches/ as commits on branch lrb
build/build.sh x64                              # or arm64 / arm for the target
build/build.sh x64 dev                          # fast-rebuild build for writing patches
```

Two build directories, same features (`build/args.gn`):

| | `out/lrb-<cpu>` (official) | `out/dev-<cpu>` (dev) |
|---|---|---|
| Settings | `args.gn`: official, ThinLTO, PGO | + `args-dev.gn`: component build, DCHECKs, line-level symbols |
| Use | Every memory measurement; packaged to `dist/` | Writing and debugging patches |
| Rebuild after a small change | 10-30 min (whole-program link) | minutes |

Never measure memory on the dev build: component builds and DCHECKs change
the footprint (Wikipedia, single-process: 546 MB PSS, against 269 MB for the
official build).

`CHROMIUM_WORKDIR` overrides where the checkout lives (default `~/chromium`).

`build/build.sh` builds `lrb`, `lrb_coordinator`, `lrb_picker` and
`content_shell` (the measurement reference, built with lrb's patches) and
writes `dist/lrb-<cpu>-<commit>.tar.zst`, the developer layout the tests
and the harness use. `build/package_release.sh <version>` turns those into
the user packages for a release (`dist/release-<version>/`): a
`low-ram-browser` launcher and `install.sh` at the top, the browser in
`lib/`, no `content_shell`, and `SHA256SUMS`. To measure it against Chromium's own
snapshot, interleaved in one sweep (the `lrb-*` configs are the `cs-*` ones
run on our build; see docs/measuring.md):

```sh
mkdir -p dist/x && tar -C dist/x --zstd -xf dist/lrb-x64-*.tar.zst
cd phase0
python3 -m lrb_harness run --binary lrb=../dist/x/content-shell/content_shell \
    --configs cs-default,lrb-default,cs-single,lrb-single
```


## Host gotchas

- **Ubuntu 23.10+** restricts unprivileged user namespaces through
  AppArmor, so sandboxed configs abort with "No usable sandbox!". Install
  the profile for this repo's binaries (edit its path if the repo isn't at
  `~/devel/low-ram-browser`):
  `sudo cp phase0/apparmor/lrb-browsers /etc/apparmor.d/ && sudo apparmor_parser -r /etc/apparmor.d/lrb-browsers`
- `install-build-deps.sh` was not needed on Ubuntu 26.04: the build uses
  Chromium's Debian sysroot.
- `gn gen` must stay rooted at our targets (`--root-target=//lrb
  --root-pattern=//lrb:all`; `build/build.sh` does it): `//chrome` targets
  assert the PDF viewer, print preview and mDNS features `args.gn` drops
  (printing itself is in, for Print to PDF). Only the
  *last* `--root-pattern` takes effect, whatever `gn help` says, hence the
  single `//lrb:all` group.
- `build/build.sh` symlinks this repo's `lrb/` to `~/chromium/src/lrb`.
- `enable_printing = true` needs `enterprise_watermark = false`: the print
  compositor's watermarks pull in the PDF viewer (`//pdf`), which
  `enable_pdf = false` leaves out.

## Moving the pinned commit

lrb follows Chromium **Stable** (Extended Stable has no Linux releases). `CHROMIUM_COMMIT` is the commit of a Stable release.

1. The current Stable release and its commit:
   `https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Linux&num=1`
   (`version`, `hashes.chromium`). Write the hash to `CHROMIUM_COMMIT`.
2. `build/setup_checkout.sh` (gclient sync to it), then
   `build/apply_patches.sh` (our patches as commits on branch `lrb`; a
   conflict stops `git am`: resolve, `git am --continue`, then re-export
   with `git format-patch`).
3. Re-check flag and feature names in `phase0/lrb_harness/configs.py` and
   arg names in `build/args.gn`; they drift between milestones (GN warns
   "Build argument has no effect" for a gone one).
4. Build both CPUs (`build/build.sh x64`, `build/build.sh arm64`), run
   `phase0/run_tests.sh`, and a memory sweep against the previous release:
   a regression in memory is a failure like a failing test.

`build/update_stable.sh` does steps 1, 2 and 4 (it checks the x64 build
against the one it replaces and fails on a page whose runs are all over 5%
dearer), leaving the result uncommitted with a summary in
`results/raw/update-<version>/SUMMARY`. `build/systemd/lrb-update-stable.timer`
runs it daily at 03:00 (install steps in the .service file).

A security point release (same milestone, about weekly) is steps 1, 2 and
4; the patches nearly always apply unchanged. A new milestone (every 4
weeks) also needs step 3 and whatever API drift breaks in `//lrb`.

## Licensing note

`proprietary_codecs = true` with `ffmpeg_branding = "Chrome"` builds in
H.264/AAC decoders. Distributing binaries with them may require patent
licenses depending on jurisdiction and volume. Building for evaluation is
fine; decide before shipping.
