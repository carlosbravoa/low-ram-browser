#!/usr/bin/env bash
# Build lrb (our browser, //lrb) and content_shell (the reference) with
# build/args.gn and package the runtime files.
#
#   build/build.sh [target_cpu] [official|dev]
#     target_cpu: x64 (default), arm64, arm, x86
#     official (default): build/args.gn, the build we measure, packaged.
#     dev: build/args.gn + build/args-dev.gn in out/dev-<cpu>, for writing
#          patches; rebuilds in minutes, not packaged, never measured.
#
# Output of an official build: dist/lrb-<cpu>-<commit>.tar.zst, unpacks to a content-shell/ dir
# laid out like the snapshot, so the Phase 0 harness can measure it with
#   --binary content_shell=<dir>/content-shell/content_shell
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="${CHROMIUM_WORKDIR:-$HOME/chromium}"
cpu="${1:-x64}"
variant="${2:-official}"
commit="$(cat "$root/CHROMIUM_COMMIT")"
case $variant in
  official) out="out/lrb-$cpu" ;;
  dev) out="out/dev-$cpu" ;;
  *) echo "unknown variant: $variant (official or dev)" >&2; exit 1 ;;
esac
export PATH="$work/depot_tools:$PATH"

cd "$work/src"
if [[ $cpu != x64 ]]; then
  # Cross builds need the target sysroot (no-op when already installed).
  build/linux/sysroot_scripts/install-sysroot.py --arch="$cpu"
fi

# Our embedder lives in this repo; the checkout sees it as //lrb, and the
# third-party Rust crates it adds (content blocking) as
# //third_party/rust/lrb, fetched and checksum-verified here.
python3 "$root/build/rust_crates.py" fetch
ln -sfn "$root/lrb" "$work/src/lrb"
ln -sfn "$root/lrb/third_party/rust" "$work/src/third_party/rust/lrb"

mkdir -p "$out"
{
  cat "$root/build/args.gn"
  [[ $variant == dev ]] && { echo; cat "$root/build/args-dev.gn"; }
  echo; echo "target_cpu = \"$cpu\""
  # Chromium ships PGO profiles for Linux x64 only (chrome/build/*.pgo.txt
  # at CHROMIUM_COMMIT): other CPUs build without PGO.
  if [[ $variant == official && $cpu != x64 ]]; then
    echo "chrome_pgo_phase = 0"
  fi
} > "$out/args.gn"
# Only our targets' graph: the default root (//:gn_all) loads //chrome
# targets that assert enable_pdf and enable_mdns, so args.gn could not drop
# those features otherwise.
gn gen "$out" --root-target=//lrb --root-pattern=//lrb:all
autoninja -C "$out" lrb lrb_coordinator lrb_picker lrb_unittests content_shell
if [[ $variant == dev ]]; then
  echo "dev build ready: $work/src/$out/lrb (and content_shell)"
  exit 0
fi

stage="$(mktemp -d)"
mkdir -p "$stage/content-shell"
for f in lrb lrb_coordinator lrb_picker lrb.pak content_shell content_shell.pak icudtl.dat v8_context_snapshot.bin snapshot_blob.bin \
         libEGL.so libGLESv2.so libvk_swiftshader.so vk_swiftshader_icd.json; do
  [[ -e $out/$f ]] && cp -a "$out/$f" "$stage/content-shell/"
done
[[ -d $out/resources ]] && cp -a "$out/resources" "$stage/content-shell/"
cp "$out/args.gn" "$stage/content-shell/args.gn"
git -C "$work/src" log -1 --format='%H %s' > "$stage/content-shell/COMMIT"
# The Chromium release under our patches: "154.0.8037.97 <commit>".
version="$(sed -n 's/^[A-Z]*=//p' "$work/src/chrome/VERSION" | paste -sd.)"
echo "$version $commit" > "$stage/content-shell/CHROMIUM"

mkdir -p "$root/dist"
tarball="$root/dist/lrb-$cpu-${commit:0:12}.tar.zst"
tar -C "$stage" --zstd -cf "$tarball" content-shell
rm -rf "$stage"
ls -l "$out/content_shell" "$tarball"
