// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_DARK_PAGES_H_
#define LRB_BROWSER_DARK_PAGES_H_

#include "base/files/file_path.h"

namespace blink::web_pref {
struct WebPreferences;
}  // namespace blink::web_pref

namespace content {
class BrowserContext;
}  // namespace content

namespace lrb {

// Dark pages, chosen per site from the menu: pages are asked for their dark
// theme (prefers-color-scheme), and Blink darkens those without one (its
// automatic dark mode, as Chrome's "Auto Dark Mode for Web Contents").
// Kept in the site's profile (kDarkPagesFile); --lrb-dark-pages turns it on
// for every site.
inline constexpr char kDarkPagesFile[] = "lrb-dark-pages";

// At startup, while blocking file access is allowed: the site's choice.
void LoadDarkPages(const base::FilePath& profile);
bool DarkPagesOn();
// Turns them on or off for this site: kept, and applied to open pages.
void SetDarkPages(content::BrowserContext* context, bool on);
// For OverrideWebPreferences.
void ApplyDarkPages(blink::web_pref::WebPreferences* prefs);

}  // namespace lrb

#endif  // LRB_BROWSER_DARK_PAGES_H_
