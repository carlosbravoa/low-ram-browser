# Packaging low-ram-browser as a snap

The snap packages a release tarball (`build/package_release.sh`, carried by
the GitHub release); snapcraft compiles nothing. `snap/snapcraft.yaml` pins
the version and the tarballs' SHA-256 for amd64 and arm64: moving to a new
release means updating those three values.

Why a snap at all: on Ubuntu and its derivatives, only programs with an
AppArmor profile may create the user namespaces Chromium's sandbox needs, so
the tarball and the .deb run in lean mode there. snapd generates the snap's
profile, and the `browser-sandbox` plug (browser-support with
`allow-sandbox: true`) grants `userns`: standard mode works from the snap,
once that plug is connected.

## Prerequisites

```sh
sudo snap install snapcraft --classic
sudo snap install lxd
sudo lxd init --auto          # once; your user must be in the lxd group
```

Snapcraft builds in an LXD container (no KVM needed). The first build
downloads core24 and the GNOME platform snap into it.

## Build

From the repository root:

```sh
SNAPCRAFT_BUILD_INFO=1 snapcraft pack
```

`SNAPCRAFT_BUILD_INFO=1` embeds `snap/manifest.yaml` (the exact staged
package versions), which the Snap Store uses to send rebuild notices when
those packages get security fixes. Put it on the same command line every
time.

The part takes `dist/release-<version>/low-ram-browser-<version>-linux-<cpu>.tar.zst`
when it exists (the machine that made the release) and downloads the
release's file otherwise; either way the SHA-256 in `snapcraft.yaml` must
match. Output: `low-ram-browser_<version>_amd64.snap`. The arm64 snap builds on
the same amd64 machine, since nothing is compiled:

```sh
SNAPCRAFT_BUILD_INFO=1 snapcraft pack --build-for arm64
```

The recipe adds Ubuntu's arm64 archive (`package-repositories`) and asks
for the `:arm64` variant of each staged library when building for arm64;
without that, snapcraft staged the amd64 libraries under aarch64 binaries
(checked 2026-11-03). The arm64 snap can't be validated in LXD here and
its tarball hasn't run on a Pi.

Check the manifest is in:

```sh
unsquashfs -l low-ram-browser_*_amd64.snap | grep -q 'squashfs-root/snap/manifest.yaml$' && echo "manifest: present"
```

## First test: devmode

```sh
sudo snap install --devmode low-ram-browser_*_amd64.snap
low-ram-browser https://en.wikipedia.org
```

Devmode logs what confinement would deny instead of denying it; use it to
find missing interfaces (`journalctl -f` or `snappy-debug`), never to ship.

## Interfaces that need `snap connect`

| Plug | For | Command |
|---|---|---|
| `browser-sandbox` | **Standard mode** (Chromium's sandbox: user namespaces, chroot). Without it the coordinator's sandbox check fails and lrb runs lean | `sudo snap connect low-ram-browser:browser-sandbox` |
| `password-manager-service` | Saved passwords and autofill in the desktop keyring (`lrb_keyring`, Secret Service). Without it lrb saves nothing | `sudo snap connect low-ram-browser:password-manager-service` |
| `camera` | getUserMedia video (lrb still asks per site) | `sudo snap connect low-ram-browser:camera` |
| `audio-record` | getUserMedia audio (lrb still asks per site) | `sudo snap connect low-ram-browser:audio-record` |
| `removable-media` | Downloads to, uploads from `/media` and `/run/media` | `sudo snap connect low-ram-browser:removable-media` |
| `u2f-devices` | WebAuthn with hardware security keys | `sudo snap connect low-ram-browser:u2f-devices` |

lrb shows these to the user: the menu's "Snap connections..." lists which
are connected, with the commands for the rest, and the first start that
finds `browser-sandbox` or `password-manager-service` missing shows "Snap
connections to make" in the bar (once; the marker is
`$XDG_DATA_HOME/lrb/.snap-notice`).

Auto-connected: `network`, `home`, `desktop`, `desktop-legacy`, `wayland`,
`x11`, `opengl`, `audio-playback`, `gsettings`, `screen-inhibit-control`
(the GNOME extension adds its own too).

## Strict confinement

Once devmode runs clean:

```sh
sudo snap remove low-ram-browser
sudo snap install --dangerous low-ram-browser_*_amd64.snap
sudo snap connect low-ram-browser:browser-sandbox
low-ram-browser https://en.wikipedia.org
```

`--dangerous` installs an unsigned local snap with real confinement. Check
the mode in the menu's "About lrb" or in the coordinator's first log line
(`snap run low-ram-browser` prints it to the terminal): "standard mode" with
the plug connected, "lean mode: ... can't create a user namespace" without.

## Snap Store

The name `low-ram-browser` is not registered yet. While the project is
alpha, `grade: devel` limits releases to the `edge` and `beta` channels:

```sh
snapcraft login
snapcraft register low-ram-browser
snapcraft upload --release=edge low-ram-browser_*_amd64.snap
```

Auto-connection of `browser-sandbox` (and the others above) in the store
takes a request on the snapcraft forum, as Firefox's and Chromium's snaps
have; until granted, users connect by hand.

## Troubleshooting

- Denials: `sudo snap install snappy-debug` then `sudo snappy-debug` in one
  terminal while running the browser in another; or
  `journalctl -k -f | grep -E 'apparmor|audit'`.
- A shell inside the snap's confinement: `snap run --shell low-ram-browser`,
  then `$SNAP/lib/lrb --lrb-sandbox-check; echo $?` (0: user namespaces
  work).
- The coordinator confines every instance itself (Landlock and a seccomp
  filter) inside the snap's confinement. snapd's seccomp template allows
  `landlock_create_ruleset`, `landlock_add_rule`, `landlock_restrict_self`,
  `seccomp` and `chroot` (checked on snapd 2.77); if an instance fails at
  `landlock_create_ruleset:` or `seccomp:` in the log, that changed.
  `lrb_coordinator --no-confine` is for debugging only.
- `journalctl -xe` for the snap's own units; there are none (no daemon).

## Known limits of the snap

- **Needs 0.1.0-alpha.8 or later.** The coordinator confines each instance
  with Landlock; until alpha.8 its read-exec list lacked `$SNAP`, so every
  instance died loading `libnspr4.so` from the snap's `usr/lib`. alpha.8
  adds `$SNAP` to the list and takes the Downloads folder from
  `$SNAP_REAL_HOME` (the real `~/Downloads`, not the snap's home).
- **Validated in LXD** (alpha.8, headless, no display): lean mode without
  `browser-sandbox`, standard mode with it (Chromium's zygote and sandboxed
  renderers ran, the shared profile filled), no AppArmor or seccomp denial.
  Not exercised there: a window, audio, the file picker, the keyring.
- **Audio.** libpulse warns "Failed to create secure directory
  ($XDG_RUNTIME_DIR/pulse)": the instance's Landlock policy lets it write
  only `dconf` under the runtime dir. Playback through the GNOME extension's
  `PULSE_SERVER` is expected to work regardless; to check on a desktop.
- **No shared certificate store.** `~/.pki/nssdb` is read only through the
  real home, which the snap's `$HOME` is not; client certificates and
  user-added authorities from Chrome or Firefox are not seen.
- **Memory for standard mode.** 3 GiB of RAM and 1.5 GiB free at start,
  besides the plug; a small LXD container exercises lean mode only.
- **arm64** is cross-compiled and not yet run on a Pi.
