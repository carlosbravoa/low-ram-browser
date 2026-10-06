// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_SAVED_WINDOWS_H_
#define LRB_BROWSER_SAVED_WINDOWS_H_

#include <string>

#include "base/functional/callback_forward.h"

class GURL;

namespace content {
class BrowserContext;
}

namespace lrb {

class Shell;

// When the coordinator closes an instance as a last resort under memory
// pressure, its windows' history is saved in the profile and restored the
// next time the site is opened.
//
// Saved per window: every history entry's URL, title and page state (form
// contents, scroll position), and which one is current. The blank entry a
// discard leaves (ActivityTracker) is skipped.

//
// Also when a site's last window is left for another site (a typed address,
// a link): that window alone is saved apart, and restored only when the
// user comes back to it with Back.

// Saves this instance's windows, then runs `done` on the UI thread.
void SaveWindows(content::BrowserContext* browser_context,
                 base::OnceClosure done);

// Saves `shell`'s window as the one `site` was left from, then runs `done`
// on the UI thread.
void SaveLeftWindow(Shell* shell,
                    const std::string& site,
                    base::OnceClosure done);

// Opens this instance's first windows: the saved ones, if any (then forgets
// them), with `restore_left` the window the site was left from, plus
// `startup_url` unless a restored window already shows it.
void OpenStartupWindows(content::BrowserContext* browser_context,
                        const GURL& startup_url,
                        bool restore_left);

}  // namespace lrb

#endif  // LRB_BROWSER_SAVED_WINDOWS_H_
