// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_ABOUT_DIALOG_H_
#define LRB_BROWSER_UI_ABOUT_DIALOG_H_

#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"

class GURL;

namespace views {
class View;
}

namespace lrb {

// The About window ("About Low RAM Browser" in the menu): the icon, what
// lrb is in a line, its version and the source it was built from, what it
// runs on (for problem reports, with a link to the issues), the
// Chromium version under it and the day Chromium released it to Stable (so
// users can tell how current its security fixes are), where the source is,
// and the copyrights and licenses (all components' in credits.html next to
// the browser, from build/build.sh). Native Views, not a page. It writes
// nothing, so a confined instance shows it itself.
// `parent`: a view of the window it belongs to. `open_tab`: opens a URL in
// a new tab of that window (the credits).
void ShowAbout(base::WeakPtr<views::View> parent,
               base::RepeatingCallback<void(const GURL&)> open_tab);

}  // namespace lrb

#endif  // LRB_BROWSER_UI_ABOUT_DIALOG_H_
