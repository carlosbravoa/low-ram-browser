// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/site.h"

#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace lrb {

std::string SiteForUrl(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
    return std::string();
  }
  std::string site = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  return site.empty() ? url.GetHost() : site;
}

}  // namespace lrb
