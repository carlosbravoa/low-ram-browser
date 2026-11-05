#!/usr/bin/env bash
# Makes the user packages for a release from build/build.sh's output
# (dist/lrb-<cpu>-<commit>.tar.zst, the developer layout): one launcher at
# the top, the browser in lib/, no content_shell (built for comparisons).
#
#   build/package_release.sh <version> [cpu ...]    # default: x64 arm64
#
# Output: dist/release-<version>/low-ram-browser-<version>-linux-<cpu>.tar.zst
# and SHA256SUMS. Each unpacks to low-ram-browser-<version>/.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?usage: build/package_release.sh <version> [cpu ...]}"
shift
cpus=("${@:-x64}")
[[ $# -eq 0 ]] && cpus=(x64 arm64)
commit="$(cat "$root/CHROMIUM_COMMIT")"
out="$root/dist/release-$version"
name="low-ram-browser-$version"
# Same bytes for the same inputs: sorted, no owners, fixed times.
mtime="@$(git -C "$root" log -1 --format=%ct)"

rm -rf "$out"
mkdir -p "$out"
for cpu in "${cpus[@]}"; do
  build="$root/dist/lrb-$cpu-${commit:0:12}.tar.zst"
  [[ -f $build ]] || { echo "no build for $cpu: $build (build/build.sh $cpu)" >&2; exit 1; }
  stage="$(mktemp -d)"
  tar -C "$stage" --zstd -xf "$build"
  built="$(sed -n 's/^lrb_version = "\(.*\)"$/\1/p' "$stage/content-shell/args.gn")"
  [[ $built == "$version" ]] || {
    echo "$cpu: the build says version '$built' (VERSION when built), not $version" >&2; exit 1; }
  pkg="$stage/$name"
  mkdir -p "$pkg/lib"
  for f in lrb lrb_coordinator lrb_picker lrb_keyring lrb.pak icudtl.dat v8_context_snapshot.bin \
           snapshot_blob.bin libEGL.so libGLESv2.so libvk_swiftshader.so \
           vk_swiftshader_icd.json credits.html CHROMIUM COMMIT args.gn; do
    [[ -e $stage/content-shell/$f ]] && mv "$stage/content-shell/$f" "$pkg/lib/"
  done
  for f in lrb lrb_coordinator lrb_picker lrb_keyring lrb.pak credits.html; do
    [[ -e $pkg/lib/$f ]] || { echo "$cpu: $f missing from $build" >&2; exit 1; }
  done
  [[ -d $stage/content-shell/resources ]] && mv "$stage/content-shell/resources" "$pkg/lib/"
  install -m 755 "$root/build/release/low-ram-browser" "$root/build/release/install.sh" "$pkg/"
  install -m 644 "$root/LICENSE" "$root/build/release/low-ram-browser.svg" "$pkg/"
  chromium="$(cut -d' ' -f1 "$pkg/lib/CHROMIUM")"
  sed -e "s/@VERSION@/$version/" -e "s/@CHROMIUM@/$chromium/" -e "s/@CPU@/$cpu/" \
    "$root/build/release/README.txt" > "$pkg/README.txt"
  tar -C "$stage" --sort=name --owner=0 --group=0 --numeric-owner --mtime="$mtime" \
    --zstd -cf "$out/$name-linux-$cpu.tar.zst" "$name"
  rm -rf "$stage"
done
(cd "$out" && sha256sum ./*.tar.zst | sed 's| \./| |' > SHA256SUMS && cat SHA256SUMS)
ls -l "$out"
