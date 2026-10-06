#!/usr/bin/env bash
# Build machine: install depot_tools and check out Chromium at CHROMIUM_COMMIT,
# shallow (no history) to keep the checkout around 30-40 GB.
#
#   build/setup_checkout.sh            # into $CHROMIUM_WORKDIR (default ~/chromium)
#
# Afterwards, once per machine: src/build/install-build-deps.sh (needs sudo).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="${CHROMIUM_WORKDIR:-$HOME/chromium}"
commit="$(cat "$root/CHROMIUM_COMMIT")"

mkdir -p "$work"
cd "$work"
if [[ ! -d depot_tools ]]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
export PATH="$work/depot_tools:$PATH"

# Written by hand instead of `fetch` so the first sync lands on the pinned
# commit directly. PGO profiles are needed by is_official_build.
if [[ ! -f .gclient ]]; then
  cat > .gclient <<'EOF'
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {
      "checkout_pgo_profiles": True,
    },
  },
]
EOF
fi

# A checkout on lrb's patch branch would have gclient rebase it onto the
# new commit; sync from a detached HEAD instead (apply_patches.sh recreates
# the branch).
if [[ -d src/.git ]]; then
  git -C src checkout -q --detach
fi
# googlesource rate-limits (HTTP 429) many parallel fetches: JOBS=4 then.
gclient sync --no-history --nohooks --revision "src@$commit" --jobs "${JOBS:-16}"
gclient runhooks

cat <<EOF

Checkout ready at $work/src (commit $commit).
Next:
  sudo $work/src/build/install-build-deps.sh   # once per machine
  $root/build/apply_patches.sh
  $root/build/build.sh
EOF
