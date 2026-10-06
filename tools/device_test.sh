#!/usr/bin/env bash
# Measures cro-minimum on a real device (a 1 GB Raspberry Pi, a 2 GB laptop)
# and writes one report file to send back. Needs the device's desktop (run it
# from a terminal there) and network access. Takes about 15-20 minutes;
# don't use the machine meanwhile.
#
#   tools/device_test.sh [path to the unpacked content-shell directory]
#
# 1. Device facts: model, OS, RAM, swap/zram, GPU driver.
# 2. One site at a time, standalone (no blocking), software compositing and
#    then the GPU: unreclaimable memory (PSS anon + shmem) per site, and
#    whether it survived.
# 3. A real session through lrb_coordinator (content blocking on): five
#    sites opened one after another; free memory, lrb's memory and processes
#    after each, and whether the coordinator discarded or closed anything.
#
# Optional: UPLOAD_URL=http://host:port/api/upload?path=dir/ uploads the
# report there (an HTTP PUT; the file name is appended).
set -uo pipefail

dir="$(cd "${1:-content-shell}" && pwd)" || { echo "no content-shell directory" >&2; exit 1; }
[[ -x $dir/lrb && -x $dir/lrb_coordinator ]] || { echo "lrb not found in $dir" >&2; exit 1; }
work="$HOME/cro-minimum-test.$$"   # on disk: a tmpfs profile would turn caches into RAM
mkdir -p "$work"
report="$PWD/cro-minimum-report-$(hostname)-$(date +%Y%m%d-%H%M).txt"
SITES=(
  "https://en.wikipedia.org/wiki/Raspberry_Pi"
  "https://www.bbc.com/news"
  "https://github.com/chromium/chromium"
  "https://www.reddit.com/r/linux/"
  "https://www.youtube.com/watch?v=aqz-KE-bpKQ"
)
SETTLE=${SETTLE:-40}   # seconds for a page to load and settle (slow devices)

say() { echo "$*" | tee -a "$report"; }

# Sum of PSS anon+shmem and total PSS (MB) over every process running a
# binary from $dir, and how many there are.
measure() {
  python3 - "$dir" <<'PY'
import os, sys
d = sys.argv[1]
anon = total = n = 0
for pid in filter(str.isdigit, os.listdir("/proc")):
    try:
        exe = os.readlink(f"/proc/{pid}/exe")
        if not exe.startswith(d + "/"):
            continue
        for line in open(f"/proc/{pid}/smaps_rollup"):
            k, v = line.split(":", 1)
            kb = int(v.split()[0]) if v.split() and v.split()[0].isdigit() else 0
            if k in ("Pss_Anon", "Pss_Shmem"):
                anon += kb
            elif k == "Pss":
                total += kb
        n += 1
    except OSError:
        pass
print(f"{anon/1024:.1f} {total/1024:.1f} {n}")
PY
}
avail() { awk '/MemAvailable/ {printf "%d", $2/1024}' /proc/meminfo; }
stop_all() {
  pkill -f "^$dir/" 2>/dev/null; sleep 3; pkill -9 -f "^$dir/" 2>/dev/null; sleep 1
}

: > "$report"
say "== cro-minimum device test, $(date -Iseconds)"
say "binary: $(cat "$dir/CHROMIUM" 2>/dev/null) / $(cut -c1-12 "$dir/COMMIT" 2>/dev/null)"
say "model: $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || cat /sys/class/dmi/id/product_name 2>/dev/null)"
say "os: $(. /etc/os-release; echo "$PRETTY_NAME") $(uname -mr)"
say "cpu: $(nproc) x $(awk -F: '/model name|Model/ {print $2; exit}' /proc/cpuinfo)"
say "session: ${XDG_SESSION_TYPE:-?} DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
say "gpu: $( (command -v glxinfo >/dev/null && glxinfo -B 2>/dev/null | grep -m1 'renderer string') || ls /dev/dri 2>/dev/null | tr '\n' ' ')"
say "memory:"; free -m | tee -a "$report"
say "swap/zram: $(swapon --show=NAME,SIZE,TYPE --noheadings 2>/dev/null | tr '\n' ';')"
say "tmp: $(findmnt -no FSTYPE /tmp 2>/dev/null)"
stop_all

say ""
say "== One site at a time, standalone, no blocking (MB: unreclaimable / total PSS / processes)"
for mode in software gpu; do
  extra=(); [[ $mode == gpu ]] && extra=(--lrb-gpu)
  say "-- $mode compositing"
  for url in about:blank "${SITES[@]}"; do
    log="$work/$mode-$(echo "$url" | tr -c 'a-zA-Z0-9' _ | cut -c1-40).log"
    XDG_CONFIG_HOME="$work/config" DBUS_SESSION_BUS_ADDRESS=disabled: \
      "$dir/lrb" --user-data-dir="$work/profile-$mode-$RANDOM" "${extra[@]}" "$url" > "$log" 2>&1 &
    pid=$!
    sleep "$SETTLE"
    if kill -0 $pid 2>/dev/null; then
      read -r anon total n < <(measure)
      say "$(printf '%-50s %7s / %7s / %s   (free %s MB)' "$url" "$anon" "$total" "$n" "$(avail)")"
    else
      say "$(printf '%-50s DIED (exit %s); log tail:' "$url" "$(wait $pid; echo $?)")"
      tail -5 "$log" | sed 's/^/    /' | tee -a "$report"
    fi
    stop_all
  done
done

say ""
say "== Session through lrb_coordinator, content blocking on, software compositing"
export XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config"
coordlog="$work/coordinator.log"
"$dir/lrb_coordinator" --verbose --socket="$work/coordinator.sock" \
  --profiles-dir="$work/sites" "${SITES[0]}" > "$coordlog" 2>&1 &
coord=$!
sleep 60   # the first start also downloads and compiles the filter lists
for i in "${!SITES[@]}"; do
  if (( i > 0 )); then
    "$dir/lrb_coordinator" --socket="$work/coordinator.sock" "${SITES[$i]}" >> "$coordlog" 2>&1
    sleep "$SETTLE"
  fi
  read -r anon total n < <(measure)
  say "$(printf '%d sites open: lrb %s MB unreclaimable, %s MB total, %s processes; free %s MB' \
        $((i + 1)) "$anon" "$total" "$n" "$(avail)")"
done
say "coordinator events:"
grep -iE "discard|closing|pressure|launch|exited|killed" "$coordlog" | tail -40 | sed 's/^/    /' | tee -a "$report"
kill $coord 2>/dev/null; stop_all
say "kernel OOM kills during the test:"
journalctl -k --since "-1h" -o cat 2>/dev/null | grep -iE "out of memory|oom-kill" | tail -5 | sed 's/^/    /' | tee -a "$report"

rm -rf "$work"
say ""
say "== done: $report"
if [[ -n ${UPLOAD_URL:-} ]]; then
  curl -sS -T "$report" "${UPLOAD_URL}$(basename "$report")" && echo "uploaded"
fi
