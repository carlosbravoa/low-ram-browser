// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_DARK_PAGES_H_
#define LRB_BROWSER_DARK_PAGES_H_

#include "base/files/file_path.h"

class GURL;

namespace blink::web_pref {
struct WebPreferences;
}  // namespace blink::web_pref

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace lrb {

// Dark pages, chosen per site from the menu: pages are asked for their dark
// theme (prefers-color-scheme), and Blink darkens those without one (its
// automatic dark mode, as Chrome's "Auto Dark Mode for Web Contents").
// Kept in the profile (kDarkPagesFile): in lean mode, where the profile is
// the site's, the file's presence means "on" (whatever it holds); in
// standard mode, one profile for every site, it lists the sites that have
// them, one per line (SiteForUrl). --lrb-dark-pages turns them on for every
// site.
inline constexpr char kDarkPagesFile[] = "lrb-dark-pages";

// At startup, while blocking file access is allowed: the profile's choices.
void LoadDarkPages(const base::FilePath& profile);
// Whether pages of `page_url`'s site get them (lean mode: any url, the
// instance's site).
bool DarkPagesOn(const GURL& page_url);
// Turns them on or off for `page_url`'s site: kept, and applied to open
// pages.
void SetDarkPages(content::BrowserContext* context,
                  const GURL& page_url,
                  bool on);
// For OverrideWebPreferences: `page_url` is the page's committed URL.
void ApplyDarkPages(blink::web_pref::WebPreferences* prefs,
                    const GURL& page_url);
// For OverrideWebPreferencesAfterNavigation: OverrideWebPreferences runs
// before a page has an address (a new window, a tab's first page), so after
// each committed navigation the page's preferences are brought in line with
// the site it is on now. Returns whether `prefs` changed.
bool RefreshDarkPages(blink::web_pref::WebPreferences* prefs,
                      const GURL& page_url);

}  // namespace lrb

#endif  // LRB_BROWSER_DARK_PAGES_H_
