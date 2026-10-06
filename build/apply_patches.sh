#!/usr/bin/env bash
# Apply patches/*.patch to the Chromium checkout, in name order, as commits on
# a local branch so they can be rebased when CHROMIUM_COMMIT moves.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="${CHROMIUM_WORKDIR:-$HOME/chromium}/src"
commit="$(cat "$root/CHROMIUM_COMMIT")"

shopt -s nullglob
patches=("$root"/patches/*.patch)
if (( ${#patches[@]} == 0 )); then
  echo "no patches to apply"
  exit 0
fi

cd "$src"
# Untracked files (the //lrb symlink, out/) are fine; edits to tracked ones are not.
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "error: $src has uncommitted changes" >&2
  exit 1
fi
git checkout -B lrb "$commit"
git am --3way "${patches[@]}"
echo "applied ${#patches[@]} patch(es) on branch lrb"
