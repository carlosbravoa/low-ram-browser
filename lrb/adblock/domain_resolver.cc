// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/adblock/domain_resolver.h"

#include <optional>
#include <string_view>

#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace lrb::adblock {

void registrable_domain_bounds(rust::Str host, size_t& start, size_t& end) {
  const std::string_view hostname(host.data(), host.size());
  start = 0;
  end = hostname.size();
  // adblock-rust hands over canonical (lowercase, punycode) hostnames.
  const std::optional<std::string_view> suffix =
      net::registry_controlled_domains::GetCanonicalHostRegistry(
          hostname, net::registry_controlled_domains::INCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  const size_t registry = suffix ? suffix->size() : 0;
  if (registry == 0 || registry + 1 >= hostname.size()) {
    return;  // no registrable domain: the whole host
  }
  // The label before the registry ("example" in "a.example.co.uk").
  const size_t before_registry = hostname.size() - registry - 1;
  const size_t dot = hostname.rfind('.', before_registry - 1);
  start = dot == std::string_view::npos ? 0 : dot + 1;
}

}  // namespace lrb::adblock
