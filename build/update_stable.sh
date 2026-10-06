#!/usr/bin/env bash
# Moves lrb to Chromium's current Stable release for Linux, if it changed:
# sync, patches, both CPUs, tests, and a memory sweep against the build it
# replaces (docs/building.md, "Moving the pinned commit"). Nothing is
# committed: the result waits for review in results/raw/update-<version>/
# (SUMMARY), and the exit status says whether it passed.
#
#   build/update_stable.sh            # check and update if needed
#   build/update_stable.sh --force    # rebuild even if CHROMIUM_COMMIT is current
#
# Run daily by build/systemd/lrb-update-stable.timer. A new milestone (every
# 4 weeks) may also need API fixes in //lrb: the build fails and says where.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
force=${1:-}
current="$(cat CHROMIUM_COMMIT)"

release="$(curl -sSf 'https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Linux&num=1')" || {
  echo "can't reach chromiumdash" >&2; exit 2; }
read -r version commit < <(python3 -c '
import json, sys
r = json.loads(sys.argv[1])[0]
print(r["version"], r["hashes"]["chromium"])' "$release")
if [[ $commit == "$current" && $force != --force ]]; then
  echo "up to date: $version ($commit)"
  exit 0
fi

out="$root/results/raw/update-$version"
mkdir -p "$out"
summary="$out/SUMMARY"
: > "$summary"
note() { echo "$*" | tee -a "$summary"; }
fail() { note "FAILED: $*"; exit 1; }
note "lrb update to Chromium $version ($commit), from $current, $(date -Iseconds)"

# The build being replaced, for the memory comparison.
rm -rf dist/x64-prev
[[ -d dist/x64 ]] && cp -a dist/x64 dist/x64-prev

echo "$commit" > CHROMIUM_COMMIT
for attempt in 1 2 3; do
  JOBS=4 build/setup_checkout.sh > "$out/sync.log" 2>&1 && break
  [[ $attempt == 3 ]] && fail "gclient sync (see sync.log)"
  sleep 600  # googlesource rate limit
done
GIT_COMMITTER_NAME="lrb updater" GIT_COMMITTER_EMAIL="lrb@localhost" \
  build/apply_patches.sh > "$out/patches.log" 2>&1 ||
  fail "a patch doesn't apply (see patches.log; resolve, git am --continue, re-export)"
note "patches apply"

build/build.sh x64 > "$out/build-x64.log" 2>&1 ||
  fail "x64 build (errors: grep -E 'error:' $out/build-x64.log)"
rm -rf dist/x64 && mkdir -p dist/x64 &&
  tar -C dist/x64 --zstd -xf "dist/lrb-x64-${commit:0:12}.tar.zst" || fail "unpacking x64"
note "x64 builds"

phase0/run_tests.sh > "$out/tests.log" 2>&1
grep -E '^== ' "$out/tests.log" | grep -v ': 0 failure' > "$out/tests-failed" || true
if [[ -s $out/tests-failed ]]; then
  note "tests failing:"; cat "$out/tests-failed" | tee -a "$summary"; fail "tests"
fi
note "tests pass ($(grep -c '^== ' "$out/tests.log"))"

if [[ -d dist/x64-prev ]]; then
  (cd phase0 && python3 -m lrb_harness run --configs lrb-lean-pressure,base-lean-pressure \
     --repeats 3 --binary lrb=../dist/x64/content-shell/content_shell \
     --binary lrb-base=../dist/x64-prev/content-shell/content_shell \
     --out "$out/sweep" > "$out/sweep.log" 2>&1 &&
   python3 -m lrb_harness report "$out/sweep" > "$out/sweep.md")
  # A page whose three runs all cost more than all three before, by over 5%.
  python3 - "$out/sweep/runs.jsonl" <<'PY' | tee -a "$summary"
import json, statistics, sys
runs = {}
for line in open(sys.argv[1]):
    r = json.loads(line)
    t = r["samples"].get("loaded", {}).get("totals")
    if not t or r.get("error"):
        continue
    mb = (t["Pss_Anon"] + t["Pss_Shmem"]) / 1024
    runs.setdefault(r["page"], {}).setdefault(r["config"].split("-")[0], []).append(mb)
worse = []
for page, by in runs.items():
    new, old = by.get("lrb", []), by.get("base", [])
    if len(new) < 2 or len(old) < 2:
        continue
    if min(new) > max(old) and statistics.median(new) > 1.05 * statistics.median(old):
        worse.append(f"{page} {statistics.median(old):.1f} -> {statistics.median(new):.1f} MB")
print("memory regressions: " + ("; ".join(worse) if worse else "none"))
sys.exit(1 if worse else 0)
PY
  [[ ${PIPESTATUS[0]} == 0 ]] || fail "memory regression (see sweep.md)"
fi

build/build.sh arm64 > "$out/build-arm64.log" 2>&1 || fail "arm64 build"
note "arm64 builds"
note "PASSED: review, then commit CHROMIUM_COMMIT (and patches/ if re-exported)"
