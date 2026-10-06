#!/usr/bin/env python3
"""Third-party Rust crates lrb needs that Chromium doesn't vendor.

  build/rust_crates.py fetch   # download + verify into lrb/third_party/rust/vendor/
  build/rust_crates.py gen     # write lrb/third_party/rust/<crate>/BUILD.gn

lrb/third_party/rust/ is linked into the checkout as //third_party/rust/lrb
(build/build.sh), so these crates may depend on Chromium's vendored ones,
whose visibility is //third_party/rust/*. Crates Chromium already vendors
(regex, serde, icu_*, ...) are used from there, never duplicated.

Versions and sha256 checksums are pinned below (adblock-rust's
dependency resolution at its pinned commit). adblock-rust itself is fetched from git at a pinned commit and
patched with patches/adblock-rust/*.patch.
"""

import hashlib
import io
import os
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUST_DIR = os.path.join(ROOT, "lrb", "third_party", "rust")
VENDOR = os.path.join(RUST_DIR, "vendor")
GN_VENDOR = "//third_party/rust/lrb/vendor"

ADBLOCK_GIT = "https://github.com/brave/adblock-rust.git"
ADBLOCK_COMMIT = "6028a6d"
ADBLOCK_VERSION = "0.13.3"

# Chromium's vendored crates, by the name lrb's crates use.
CHROMIUM = {
    "serde_core": "//third_party/rust/serde_core/v1:lib",
    "smallvec": "//third_party/rust/smallvec/v1:lib",
    "utf8_iter": "//third_party/rust/utf8_iter/v1:lib",
    "icu_normalizer": "//third_party/rust/icu_normalizer/v2:lib",
    "icu_properties": "//third_party/rust/icu_properties/v2:lib",
    "itertools": "//third_party/rust/itertools/v0_14:lib",
    "serde": "//third_party/rust/serde/v1:lib",
    "serde_json": "//third_party/rust/serde_json/v1:lib",
    "memchr": "//third_party/rust/memchr/v2:lib",
    "base64": "//third_party/rust/base64/v0_22:lib",
    "thiserror": "//third_party/rust/thiserror/v2:lib",
}

# name: version, sha256, epoch, edition, features, deps (crate names)
#
# The regex family is vendored here although Chromium has it: Chromium marks
# regex testonly (production Chromium uses RE2) and its aho_corasick target
# forbids the unsafe code the crate needs. Features mirror Chromium's.
_UNICODE = ["unicode", "unicode-age", "unicode-bool", "unicode-case", "unicode-gencat",
            "unicode-perl", "unicode-script", "unicode-segment"]
CRATES = {
    # Chromium builds bitflags without its serde feature, which adblock needs.
    "bitflags": ("2.13.2", "3ded4057c258ba199e2d26386d3af3780957ecaee6c4ef4041c6b4b8b97c0b06",
                 "2", "2021", ["serde", "std"], ["serde_core"]),
    "aho-corasick": ("1.1.5", "c982642fa9e8606056828ee9a8505737230110bb1099153c79efe865c59d12ba",
                     "1", "2021", ["perf-literal", "std"], ["memchr"]),
    "regex-syntax": ("0.8.11", "d6f6ff9a378485b298a5286656da665ba74413d36db0979633275d2e708145d4",
                     "0.8", "2021", ["default", "std"] + _UNICODE, []),
    "regex-automata": ("0.4.18", "ad8553b9b26413251cbf30e620595c7a41b3887f03da04579c0e6b0d6a06b4b2",
                       "0.4", "2021",
                       ["alloc", "dfa-onepass", "dfa-search", "hybrid", "meta", "nfa-backtrack",
                        "nfa-pikevm", "nfa-thompson", "perf-inline", "perf-literal",
                        "perf-literal-multisubstring", "perf-literal-substring", "std", "syntax",
                        "unicode-word-boundary"] + _UNICODE,
                       ["aho-corasick", "memchr", "regex-syntax"]),
    "regex": ("1.13.1", "f020237b6c8eed93db2e2cb53c00c60a8e1bc73da7d073199a1180401450218d",
              "1", "2021",
              ["default", "perf", "perf-backtrack", "perf-cache", "perf-dfa", "perf-inline",
               "perf-literal", "perf-onepass", "std"] + _UNICODE,
              ["aho-corasick", "memchr", "regex-automata", "regex-syntax"]),
    "flatbuffers": ("25.12.19", "35f6839d7b3b98adde531effaf34f0c2badc6f4735d26fe74709d8e513a96ef3",
                    "25", "2018", ["default", "std"], ["bitflags"]),
    "seahash": ("4.1.0", "1c107b6f4780854c8b126e228ea8869f4d7b71260f962fefb57b996b8959ba6b",
                "4", "2015", [], []),
    "rustc-hash": ("2.1.3", "6b1e7f9a428571be2dc5bc0505c13fb6bf936822b894ec87abf8a08a4e51742d",
                   "2", "2021", ["default", "std"], []),
    "arrayvec": ("0.7.8", "d3fb67a6e08acf24fdeccbac2cb6ac4305825bd1f117462e0e6f2f193345ad56",
                 "0.7", "2018", ["default", "std"], []),
    "percent-encoding": ("2.3.2", "9b4f627cb1b25917193a259e49bdad08f671f8d9708acfd5fe0a8c1455d87220",
                         "2", "2018", ["alloc", "default", "std"], []),
    "idna_adapter": ("1.2.2", "cb68373c0d6620ef8105e855e7745e18b0d00d3bdb07fb532e434244cdb9a714",
                     "1", "2024", ["compiled_data"], ["icu_normalizer", "icu_properties"]),
    "idna": ("1.1.0", "3b0875f23caa03898994f6ddc501886a45c7d3d62d04d2d90788d47be1b1e4de",
             "1", "2018", ["alloc", "compiled_data", "default", "std"],
             ["idna_adapter", "smallvec", "utf8_iter"]),
}

# adblock: features without its embedded public suffix list (Chromium's is
# plugged in instead) and without single-thread (the engine is Sync).
ADBLOCK = (ADBLOCK_VERSION, "0.13", "2024", ["full-regex-handling", "resource-assembler"],
           ["arrayvec", "base64", "bitflags", "flatbuffers", "idna", "itertools", "memchr",
            "percent-encoding", "regex", "rustc-hash", "seahash", "serde", "serde_json",
            "thiserror"])


def crate_dir(name, version):
    return os.path.join(VENDOR, f"{name}-{version}")


def fetch():
    os.makedirs(VENDOR, exist_ok=True)
    for name, (version, sha256, *_rest) in CRATES.items():
        dest = crate_dir(name, version)
        if os.path.isdir(dest):
            continue
        url = f"https://static.crates.io/crates/{name}/{name}-{version}.crate"
        data = urllib.request.urlopen(url, timeout=60).read()
        got = hashlib.sha256(data).hexdigest()
        if got != sha256:
            sys.exit(f"{name}-{version}: sha256 {got}, expected {sha256}")
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            tar.extractall(VENDOR, filter="data")
        print(f"fetched {name}-{version}")

    dest = crate_dir("adblock", ADBLOCK_VERSION)
    if not os.path.isdir(dest):
        subprocess.run(["git", "clone", "-q", ADBLOCK_GIT, dest], check=True)
        subprocess.run(["git", "-C", dest, "checkout", "-q", ADBLOCK_COMMIT], check=True)
        patches = sorted(os.path.join(ROOT, "patches", "adblock-rust", p)
                         for p in os.listdir(os.path.join(ROOT, "patches", "adblock-rust"))
                         if p.endswith(".patch"))
        subprocess.run(["git", "-C", dest, "-c", "user.name=lrb", "-c", "user.email=lrb@localhost",
                        "am", "-q"] + patches, check=True)
        print(f"fetched adblock {ADBLOCK_COMMIT} + {len(patches)} patch(es)")


def gn_list(items, indent=4):
    pad = " " * indent
    return "[\n" + "".join(f'{pad}"{i}",\n' for i in items) + " " * (indent - 2) + "]"


def target_label(name):
    if name in CHROMIUM:
        return CHROMIUM[name]
    return f"//third_party/rust/lrb/{name}:lib"


def write_build(name, version, epoch, edition, features, deps, visibility, extra="",
                testonly=False):
    src = crate_dir(name, version)
    root = os.path.join(src, "src", "lib.rs")
    sources = []
    for d, _dirs, files in os.walk(os.path.join(src, "src")):
        sources += [os.path.join(d, f) for f in files if f.endswith(".rs")]
    rel = lambda p: GN_VENDOR + "/" + os.path.relpath(p, VENDOR)
    crate_name = name.replace("-", "_")
    out = os.path.join(RUST_DIR, name, "BUILD.gn")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        f.write(f'''# Generated by build/rust_crates.py gen. Do not edit.

import("//build/rust/cargo_crate.gni")

cargo_crate("lib") {{
  crate_name = "{crate_name}"
  epoch = "{epoch}"
  crate_type = "rlib"
  crate_root = "{rel(root)}"
  sources = {gn_list(sorted(rel(s) for s in sources))}
  inputs = []
  build_native_rust_unit_tests = false
  edition = "{edition}"
  cargo_pkg_name = "{name}"
  cargo_pkg_version = "{version}"
  allow_unsafe = true
  features = {gn_list(features)}
  deps = {gn_list([target_label(d) for d in deps])}
  visibility = {gn_list(visibility)}
{"  testonly = true" + chr(10) if testonly else ""}
  # Third-party code: no chromium_code warnings, no coverage.
  library_configs -= [
    "//build/config/compiler:chromium_code",
    "//build/config/coverage:default_coverage",
  ]
{extra}}}
''')
    print(f"wrote {os.path.relpath(out, ROOT)}")


def gen():
    for name, (version, _sha, epoch, edition, features, deps) in CRATES.items():
        visibility = ["//third_party/rust/lrb/*"]
        if name == "regex":
            visibility.append("//lrb/*")  # lrb/adblock parses uBO's resource map
        write_build(name, version, epoch, edition, features, deps, visibility)
    version, epoch, edition, features, deps = ADBLOCK
    write_build("adblock", version, epoch, edition, features, deps,
                ["//third_party/rust/lrb/*", "//lrb/*"])


if __name__ == "__main__":
    {"fetch": fetch, "gen": gen}[sys.argv[1]]()
