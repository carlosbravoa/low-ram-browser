low-ram-browser @VERSION@ (Chromium @CHROMIUM@, @CPU@)
https://github.com/carlosbravoa/low-ram-browser

To try it, run:

    ./low-ram-browser                      (or: ./low-ram-browser https://example.com)

To add it to your applications menu and your PATH (no root needed):

    ./install.sh                           (undo: ./install.sh --uninstall)

Each site opens in its own window and process, confined so it can't reach
your files; uploads and downloads go through your desktop's file picker.
The first start downloads the ad-blocking lists in the background (about
a minute).

Your data:  ~/.local/share/lrb (one profile per site)
Settings:   ~/.config/lrb/settings.json (or the menu's Settings)

lib/ holds the browser itself; you don't need to run anything in it.
Version, source and licenses: the menu's "About Low RAM Browser". Licenses: LICENSE
(lrb's), lib/credits.html (every component's, Chromium's included).
