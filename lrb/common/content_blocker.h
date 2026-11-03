// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_CONTENT_BLOCKER_H_
#define LRB_COMMON_CONTENT_BLOCKER_H_

#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"

class GURL;

namespace lrb {

namespace adblock {
struct Engine;  // lrb/adblock/lib.rs
}

// Maps an engine file (ListUpdater writes them: lrb's container around
// adblock-rust's serialized engine) and loads the engine in place, with the
// surrogates next to it if `with_resources`. Both stay for the rest of the
// process. Null, with a warning, if the file can't be used. Blocking.
const adblock::Engine* LoadEngineFile(const base::FilePath& path,
                                      bool with_resources);

// Ad and tracker blocking with adblock-rust (lrb/adblock). One engine per
// process, used from any thread, in place from a read-only engine file
// that every instance maps (so the rules aren't copied into each).
class ContentBlocker {
 public:
  struct Decision {
    bool block = false;
    // A surrogate to answer with instead of failing visibly (a data: URL),
    // when the matching rule has one ($redirect).
    std::string redirect;
  };

  // Loads the engine file (lrb's container around adblock-rust's
  // serialization; ListUpdater writes it). Blocking. Usually called once at
  // startup, before any request; also later, from the thread pool, when the
  // coordinator has just built the lists (requests after it are checked).
  // Until then, and if it fails, nothing is blocked.
  static bool Load(const base::FilePath& path);

  // The same from an open read-only engine file and, if valid, its
  // resources file (the surrogates and the scriptlets): renderers get both
  // as descriptors from the browser. Blocking.
  static bool Load(base::File file, base::File resources);

  // Whether a request from a page at `source` should be blocked.
  static Decision Check(const GURL& url,
                        const GURL& source,
                        network::mojom::RequestDestination destination,
                        const std::string& method);

  // A user stylesheet hiding the ad containers the cosmetic rules name for
  // `url`'s site; empty if none (or with the lean, network-only lists).
  static std::string CosmeticCss(const GURL& url);

  // The scriptlets for `url`'s site, as one script to run in the page's main
  // world before its own scripts; empty if none.
  static std::string Scriptlets(const GURL& url);

  static bool loaded();

  // Lean mode: the user turned blocking off for this instance's site (each
  // instance is one site): nothing is blocked, hidden or injected. Kept as
  // a marker file in the site's profile (kBlockingOffFile).
  static void SetEnabled(bool enabled);
  static bool enabled();

  // Whether blocking applies to a page at `page_url`: enabled(), and the
  // page's site isn't one the user turned it off for. Check, CosmeticCss
  // and Scriptlets ask this.
  static bool EnabledFor(const GURL& page_url);

  // Standard mode, where one instance shows every site: blocking off (or on
  // again) for one site. Only this process learns it: the browser passes
  // the list to the renderers it starts (--lrb-blocking-off-sites), so a
  // site's pages must get a new renderer for a change to reach its cosmetic
  // rules (LrbContentBrowserClient::ReloadSiteBypassingCache).
  static void SetSiteEnabled(const std::string& site, bool enabled);
  // The sites blocking is off for, sorted.
  static std::vector<std::string> DisabledSites();
  // The disabled sites from --lrb-blocking-off-sites (comma-separated), in
  // place of any known so far.
  static void SetDisabledSitesFromSwitch(const std::string& value);
  // The browser keeps the list in its profile (kBlockingOffSitesFile), one
  // site per line. Both block.
  static void ReadDisabledSites(const base::FilePath& file);
  static bool WriteDisabledSites(const base::FilePath& file);
};

// In the site's profile directory: blocking is off for the site.
inline constexpr char kBlockingOffFile[] = "lrb-blocking-off";

// Standard mode, in the standard profile: the sites blocking is off for.
inline constexpr char kBlockingOffSitesFile[] = "lrb-blocking-off-sites";

}  // namespace lrb

#endif  // LRB_COMMON_CONTENT_BLOCKER_H_
