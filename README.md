# low-ram-browser (lrb)

A Chromium-based browser with the smallest memory footprint that still
browses the mainstream web, for machines where every kilobyte counts:
2 GB laptops, a 1 GB Raspberry Pi, or any computer whose RAM other programs
already hold.

![lrb on Wikipedia](docs/img/window-wikipedia.png)

It is built on Chromium's `//content` layer, not the Chrome product, and
follows Chromium's Stable releases. Every change is judged by measured
memory, and keeping mainstream sites working is the hard constraint. The
numbers and how to reproduce them are in [MEASUREMENTS.md](MEASUREMENTS.md).

**Status: experimental.** It is usable day to day. Each site's process runs
under whole-browser OS confinement (Landlock, seccomp) in place of
Chromium's per-renderer sandbox; see [Security](#security).

## How it saves memory

- **One site per window, one process per window.** Each site gets its own
  single-process browser instance and its own profile, so sites never share
  a process or a cookie jar. Tabs exist within a site's window. Leaving a
  site replaces the window. ([docs/design.md](docs/design.md))
- **Pressure-driven.** Background tabs and windows sleep under memory
  pressure, keeping their history, and the oldest close only as a last
  resort. A small coordinator process that never loads web content decides.
- **Content blocking** with Brave's adblock-rust (uBlock Origin-compatible
  lists), built once and shared read-only by every instance.
- **No autoplay**, muted or not, before the user interacts with the page.
- **Lean defaults:** software compositing (the GPU is an option),
  low-end-device mode, V8 optimizing for size, no back-forward cache, no
  prerendering, and Stable's feature defaults without Chromium's
  experiment test config.
- **A native slim bar** (Views, about 2 MB per window) rather than an HTML
  one.

## Security

- **Sites are kept apart by processes and profiles:** a site never shares
  a process or its cookies and storage with another site.
- **Every site's process is confined** by `lrb_coordinator` before it
  starts: Landlock limits it to its own profile, the browser's files, and
  what a desktop browser needs (system libraries, fonts, themes, sound,
  camera, GPU), not your home directory or other sites' profiles. seccomp
  refuses system calls a browser never needs, and `no_new_privs` keeps
  setuid programs from giving privileges back. On a kernel without
  Landlock, seccomp and `no_new_privs` still apply.
- **Your files only through the file picker:** uploads and downloads go
  through the desktop's own file dialog (XDG portal), shown by the
  coordinator, which hands the site only the file you chose. Upload copies
  are deleted when the site's process exits.
- **Third-party content inside a page** (ads, embeds) shares that page's
  process, unlike in Chrome; content blocking removes most of it.

Details and the comparison with Chrome: [docs/design.md](docs/design.md#security-model).

## On a Raspberry Pi 3 (1 GB)

A Raspberry Pi 3 Model B+ (905 MB usable, 905 MB zram, Debian 13 64-bit)
running the arm64 alpha with `tools/device_test.sh`. Memory that can't be
paged out (anonymous + shared), MB, one site at a time without content
blocking:

| page | software rendering | GPU |
|---|---:|---:|
| blank | 32 | 39 |
| Wikipedia | 86 | 93 |
| GitHub | 105 | 125 |
| Reddit | 123 | 134 |
| BBC News | 155 | 161 |
| YouTube | 254 | 309 |

Five sites opened one after another through `lrb_coordinator`, content
blocking on:

| sites open | lrb | free memory |
|---|---:|---:|
| 1 | 87 MB | 546 MB |
| 2 | 169 MB | 422 MB |
| 3 | 285 MB | 302 MB |
| 4 | 277 MB | 311 MB |
| 5 | 325 MB | 242 MB |

Memory pressure began at the third site; the coordinator put the least
recently used windows to sleep, so five open sites cost about what three
did. Every site loaded, nothing crashed, and the kernel killed nothing.
Not measured yet: load times and how smooth scrolling and video are with
and without the GPU. More numbers, and how to reproduce them:
[MEASUREMENTS.md](MEASUREMENTS.md).

## Safety and privacy defaults

- No permission without the user saying yes. Camera, microphone and
  clipboard reading are asked about in the window and remembered per site.
  Everything else is refused.
- Downloads always ask where to save.
- No remote-debugging server unless explicitly asked for.
- Origin trials use Chromium's production key.

## Download

Pre-releases for 64-bit x86 and ARM Linux (a Raspberry Pi with the 64-bit
OS) are on the [releases page](https://github.com/carlosbravoa/low-ram-browser/releases).

```sh
tar --zstd -xf low-ram-browser-<version>-linux-x64.tar.zst
cd low-ram-browser-<version>
./low-ram-browser            # try it from here
./install.sh                 # optional: applications menu and PATH, no root
```

## Building and running

Linux x86-64 build machine, about 100 GB of disk and 32 GB of RAM. Builds
x64 and arm64. See [docs/building.md](docs/building.md).

```sh
build/setup_checkout.sh      # depot_tools + Chromium at CHROMIUM_COMMIT
build/apply_patches.sh       # patches/ onto the checkout
build/build.sh x64           # -> dist/lrb-x64-<commit>.tar.zst
tar --zstd -xf dist/lrb-x64-*.tar.zst
content-shell/lrb_coordinator https://en.wikipedia.org
```

Tests: `phase0/run_tests.sh` (some open windows on `$DISPLAY`, some need
network access).

## Layout

| Path | What |
|---|---|
| `lrb/` | The browser: a `//content` embedder (`//lrb` in the Chromium checkout), its coordinator and content blocking. |
| `patches/` | The only changes to Chromium itself, as `git am` patches against `CHROMIUM_COMMIT`. |
| `build/` | Checkout, patch, build, package, and follow Stable (`update_stable.sh`). |
| `phase0/` | Memory harness (Python standard library only, runs on target devices) and the test suite. |
| `CHROMIUM_COMMIT` | The Chromium Stable release lrb builds on. |
| `CHROMIUM_REVISION` | The Chromium snapshot used as a measurement reference (`phase0/fetch_prebuilt.sh`). |
| `docs/` | Design, building, measuring. |

## License

BSD-3-Clause ([LICENSE](LICENSE)). Third-party code and licenses:
[THIRD_PARTY.md](THIRD_PARTY.md).
