#!/bin/sh
# Installs low-ram-browser for the current user: no root, nothing outside
# your home directory.
#
#   ./install.sh               copy to ~/.local/opt/low-ram-browser, add
#                              `low-ram-browser` to ~/.local/bin and an
#                              entry to the applications menu
#   ./install.sh --uninstall   remove those (keeps your sites' data in
#                              ~/.local/share/lrb and settings in ~/.config/lrb)
set -eu
here="$(dirname "$(readlink -f "$0")")"
opt="$HOME/.local/opt/low-ram-browser"
bin="$HOME/.local/bin/low-ram-browser"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
entry="$apps/low-ram-browser.desktop"

if [ "${1:-}" = --uninstall ]; then
  rm -rf "$opt"
  rm -f "$bin" "$entry"
  command -v update-desktop-database >/dev/null &&
    update-desktop-database -q "$apps" 2>/dev/null || true
  echo "Uninstalled. Your data is still in ~/.local/share/lrb and ~/.config/lrb."
  exit 0
fi

if [ "$here" != "$opt" ]; then
  rm -rf "$opt.new"
  mkdir -p "$opt.new"
  cp -a "$here/." "$opt.new/"
  rm -rf "$opt"
  mv "$opt.new" "$opt"
fi
mkdir -p "$(dirname "$bin")" "$apps"
ln -sfn "$opt/low-ram-browser" "$bin"
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=Low RAM Browser
GenericName=Web Browser
Comment=Browse the web with as little memory as possible
Exec=$opt/low-ram-browser %u
Icon=web-browser
Terminal=false
Categories=Network;WebBrowser;
MimeType=x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
EOF
command -v update-desktop-database >/dev/null &&
  update-desktop-database -q "$apps" 2>/dev/null || true

echo "Installed to $opt."
echo "Start it from the applications menu (Low RAM Browser) or run: low-ram-browser"
case ":$PATH:" in
  *":$HOME/.local/bin:"*) ;;
  *) echo "(~/.local/bin isn't on your PATH: log out and in, or run $bin)" ;;
esac
