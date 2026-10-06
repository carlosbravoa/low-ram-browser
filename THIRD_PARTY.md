# Third-party code

| What | Where it comes from | License |
|---|---|---|
| Chromium | Checked out by `build/setup_checkout.sh` at `CHROMIUM_COMMIT`; changed only by `patches/*.patch` | BSD-3-Clause (Chromium), plus its third-party components' licenses |
| Code copied from Chromium's `content/shell` | `lrb/` files that carry "Copyright The Chromium Authors" next to ours | BSD-3-Clause |
| adblock-rust (Brave) | Fetched by `build/rust_crates.py` at a pinned commit, patched with `patches/adblock-rust/` | MPL-2.0 |
| Rust crates adblock-rust needs that Chromium doesn't vendor (regex family, flatbuffers, seahash, ...) | Fetched by `build/rust_crates.py`, versions and sha256 pinned in it | MIT / Apache-2.0 (each crate's own) |
| Filter lists (EasyList, EasyPrivacy, uBlock Origin's) | Downloaded by lrb at run time, never shipped | Each list's own (GPL-3.0 / CC BY-SA 3.0) |

Binaries built with `proprietary_codecs = true` (`build/args.gn`) include
H.264/AAC decoders; distributing them may require patent licenses
depending on jurisdiction and volume.
