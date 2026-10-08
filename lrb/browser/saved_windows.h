// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_SAVED_WINDOWS_H_
#define LRB_BROWSER_SAVED_WINDOWS_H_

#include <string>

#include "base/functional/callback_forward.h"
#include "base/values.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"

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

// The site's session: the same file, kept current while the site is open
// (SessionChanged, a moment after tabs or pages change), so a crash, a kill
// or a logout loses nothing: the windows come back the next time the site
// opens. Closing them yourself empties it.

// Tabs or pages changed: saves the windows shortly (one write for a burst).
void SessionChanged(content::BrowserContext* browser_context);
// The user closed the last window: the session goes.
void SessionClosedByUser(content::BrowserContext* browser_context);
// From now on the session stays as saved: closing to free memory (the
// windows go, but come back).
void KeepSession();

// One tab's history, as saved (for reopening a closed tab).
base::DictValue SerializeTab(Shell* shell);
// Loads `tab` (its saved history, else its address) into a new Shell;
// where it goes is the platform delegate's.
Shell* LoadSavedTab(content::BrowserContext* browser_context,
                    const LrbPlatformDelegate::TabState& tab);

// Saves this instance's windows, then runs `done` on the UI thread.
void SaveWindows(content::BrowserContext* browser_context,
                 base::OnceClosure done);

// Saves `shell`'s window as the one `site` was left from, then runs `done`
// on the UI thread.
void SaveLeftWindow(Shell* shell,
                    const std::string& site,
                    base::OnceClosure done);

// Opens this instance's first windows: the saved ones, if any, with
// `restore_left` the window the site was left from (then forgotten), plus
// `startup_url` unless a restored window already shows it, or, with
// `resume`, unless any window came back (lrb started on the last site).
void OpenStartupWindows(content::BrowserContext* browser_context,
                        const GURL& startup_url,
                        bool restore_left,
                        bool resume);

}  // namespace lrb

#endif  // LRB_BROWSER_SAVED_WINDOWS_H_
