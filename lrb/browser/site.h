// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_SITE_H_
#define LRB_BROWSER_SITE_H_

#include <string>

class GURL;

namespace lrb {

// The site a URL belongs to: its registrable domain ("mail.google.com" →
// "google.com"), or the host when there is none (IP addresses, localhost).
// Empty for URLs that aren't http(s). One site = one window = one instance.
std::string SiteForUrl(const GURL& url);

}  // namespace lrb

#endif  // LRB_BROWSER_SITE_H_
