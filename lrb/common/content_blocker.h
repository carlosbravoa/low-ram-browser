// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_CONTENT_BLOCKER_H_
#define LRB_COMMON_CONTENT_BLOCKER_H_

#include <string>

#include "base/files/file_path.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"

class GURL;

namespace lrb {

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
  // serialization; tools/adblock-feasibility writes it). Blocking. Call
  // once, before any request; until then, and if it fails, nothing is
  // blocked.
  static bool Load(const base::FilePath& path);

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

  // The user turned blocking off for this site (each instance is one
  // site): nothing is blocked, hidden or injected. Kept as a marker file
  // in the site's profile (kBlockingOffFile).
  static void SetEnabled(bool enabled);
  static bool enabled();
};

// In the site's profile directory: blocking is off for the site.
inline constexpr char kBlockingOffFile[] = "lrb-blocking-off";

}  // namespace lrb

#endif  // LRB_COMMON_CONTENT_BLOCKER_H_
