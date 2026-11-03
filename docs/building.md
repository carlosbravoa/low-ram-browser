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
| Use | Releases; packaged to `dist/` | Writing and debugging patches |
| Rebuild after a small change | 10-30 min (whole-program link) | minutes |

The dev build's memory isn't the release's: component builds and DCHECKs
double it (Wikipedia: 546 MB against 269 MB).

`CHROMIUM_WORKDIR` overrides where the checkout lives (default `~/chromium`).

`build/build.sh` builds `lrb`, `lrb_coordinator`, `lrb_picker`, `lrb_keyring` and
`content_shell` (Chromium's own shell with lrb's patches, for comparisons;
not in the release packages) and writes `dist/lrb-<cpu>-<commit>.tar.zst`,
a developer layout: unpack it and run `content-shell/lrb_coordinator`.
`build/package_release.sh <version>` turns those into
the user packages for a release (`dist/release-<version>/`): a
`low-ram-browser` launcher, `install.sh` and `LICENSE` at the top, the
browser in `lib/`, no `content_shell`, and `SHA256SUMS`. `<version>` must
be what `VERSION` said when the build was made: About shows it, with the
source's commit and the Chromium release date from `CHROMIUM_RELEASE`.
The build also writes `credits.html`, the licenses of every component in
lrb's dependency graph (Chromium's `tools/licenses/licenses.py`, lrb's
Rust crates included through their `README.chromium`).

## Host gotchas

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
   (`version`, `hashes.chromium`, `time`). Write the hash to
   `CHROMIUM_COMMIT`, and the version and the release's day to
   `CHROMIUM_RELEASE` (`155.0.8059.39 2026-10-06`).
2. `build/setup_checkout.sh` (gclient sync to it), then
   `build/apply_patches.sh` (our patches as commits on branch `lrb`; a
   conflict stops `git am`: resolve, `git am --continue`, then re-export
   with `git format-patch`).
3. Re-check arg names in `build/args.gn`; they drift between milestones
   (GN warns "Build argument has no effect" for a gone one). A new
   milestone (every 4 weeks) may also need API fixes in `//lrb`; a
   security point release (about weekly) nearly always builds unchanged.
4. Build both CPUs (`build/build.sh x64`, `build/build.sh arm64`).

## Licensing note

`proprietary_codecs = true` with `ffmpeg_branding = "Chrome"` builds in
H.264/AAC decoders. Distributing binaries with them may require patent
licenses depending on jurisdiction and volume. Building for evaluation is
fine; decide before shipping.

## Standard mode needs user namespaces

In standard mode ([docs/design.md](design.md#modes)) lrb runs Chromium's
own renderer sandbox, which creates user namespaces. Ubuntu 23.10 and later
(and derivatives such as Linux Mint and Pop!_OS) let only programs with an
AppArmor profile use them; Chrome and Firefox work there because Ubuntu
ships a profile for their install paths or because they are snaps, whose
profiles snapd generates. lrb ships no profile and no setuid helper: the
tarball and the .deb run in lean mode on those systems, which is still
confined (Landlock, seccomp), and `lrb_coordinator` logs why
(`lrb --lrb-sandbox-check` fails at start). Standard mode on Ubuntu will
come with the snap package. Elsewhere (Debian, Fedora, Arch, Raspberry Pi
OS) user namespaces are open and standard mode works from the tarball.

A build machine on Ubuntu that wants to run or test standard mode needs a
profile allowing `userns` for the build's `lrb` binary, as Chromium's
[apparmor-userns-restrictions.md](https://chromium.googlesource.com/chromium/src/+/main/docs/security/apparmor-userns-restrictions.md)
describes for developer builds. That is a development setup, not something
users install.
