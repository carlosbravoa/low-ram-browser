#!/usr/bin/env bash
# Download the Chromium snapshot build matching CHROMIUM_REVISION:
# content_shell (our future base) and full Chromium (reference for the cost of
# the //chrome layer). Unpacks into prebuilt/<revision>/.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
rev="${1:-$(cat "$root/CHROMIUM_REVISION")}"
platform="${PLATFORM:-Linux_x64}"
base="https://commondatastorage.googleapis.com/chromium-browser-snapshots/$platform/$rev"
dest="$root/prebuilt/$rev"

mkdir -p "$dest"
cd "$dest"
for f in REVISIONS content-shell.zip chrome-linux.zip; do
  if [[ $f == *.zip && -d ${f%.zip} ]]; then
    echo "already unpacked: ${f%.zip}"
    continue
  fi
  echo "fetching $base/$f"
  curl -sSfLO "$base/$f"
  if [[ $f == *.zip ]]; then
    unzip -q -o "$f"
    rm "$f"
  fi
done

expected="$(cat "$root/CHROMIUM_COMMIT")"
got="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["got_revision"])' REVISIONS)"
if [[ $rev == "$(cat "$root/CHROMIUM_REVISION")" && $got != "$expected" ]]; then
  echo "warning: snapshot commit $got does not match CHROMIUM_COMMIT $expected" >&2
fi
echo "ok: $dest (commit $got)"
