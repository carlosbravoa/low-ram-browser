#!/bin/bash
# Runs every lrb test against a build (default: dist/x64/content-shell).
#
#   phase0/run_tests.sh [dir with lrb and lrb_coordinator]
#
# Headless tests run without a session bus (as the harness does); the X11
# ones open windows on $DISPLAY (XWayland works) and need it. Some need
# network access. One line per test; then crash traps the kernel logged
# (CHECK failures in official builds) and lrb windows left open.
set -u
here=$(cd "$(dirname "$0")" && pwd)
dir=$(cd "${1:-$here/../dist/x64/content-shell}" && pwd)
cd "$here"
t0=$(date +%s)
run() {  # timeout, test, argument
  echo "== $2: $(timeout "$1" python3 "$2" "$3" 2>&1 | grep -E 'FAIL|failure|Error' | tr '\n' ' ')"
}
for t in test_permissions.py test_dialogs.py test_downloads.py test_uploads.py test_devtools_off.py test_identity.py; do
  DBUS_SESSION_BUS_ADDRESS=disabled: run 150 "$t" "$dir/lrb"
done
for t in test_coordinator.py test_close.py test_discard.py test_tab_sleep.py test_broker.py; do
  run 400 "$t" "$dir"
done
for t in test_site_windows.py test_back_x11.py test_permission_prompt_x11.py \
         test_close_warning_x11.py test_portal_picker.py test_context_menu_x11.py \
         test_find_zoom_x11.py test_sign_in_x11.py test_blocking_switch_x11.py \
         test_settings_x11.py test_tabs_x11.py; do
  run 300 "$t" "$dir/lrb"
done
echo "crash traps: $(journalctl -k --since "@$t0" -o cat 2>/dev/null | grep -c 'trap int3')"
python3 -c "from lrb_harness import x11; print('lrb windows left open:', len(x11.windows()))"
