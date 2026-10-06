// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_SITE_WINDOW_THROTTLE_H_
#define LRB_BROWSER_SITE_WINDOW_THROTTLE_H_

#include "base/memory/raw_ref.h"
#include "content/public/browser/navigation_throttle.h"

namespace lrb {

// Keeps an instance to its one site. A user-initiated main-frame navigation
// to another site (typed URL, link click) is cancelled here and opened in
// that site's own window instead. Navigations within a flow (scripts, form
// submissions, server redirects, back/forward, reload) pass through, so
// logins and payments that bounce through other sites keep working.
//
// An instance without a site yet takes the site of its first http(s) page.
// Under a coordinator it instead hands that page to the coordinator and
// closes, so the page opens with its site's profile.
class LrbContentBrowserClient;

class SiteWindowThrottle : public content::NavigationThrottle {
 public:
  SiteWindowThrottle(content::NavigationThrottleRegistry& registry,
                     LrbContentBrowserClient& client);
  ~SiteWindowThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  const char* GetNameForLogging() override;

 private:
  const raw_ref<LrbContentBrowserClient> client_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_SITE_WINDOW_THROTTLE_H_
