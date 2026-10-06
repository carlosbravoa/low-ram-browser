// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_ADBLOCK_DOMAIN_RESOLVER_H_
#define LRB_ADBLOCK_DOMAIN_RESOLVER_H_

#include <cstddef>

#include "third_party/rust/cxx/v1/cxx.h"

namespace lrb::adblock {

// For adblock-rust: where the registrable domain (eTLD+1) of `host` starts
// and ends, from Chromium's public suffix list.
void registrable_domain_bounds(rust::Str host, size_t& start, size_t& end);

}  // namespace lrb::adblock

#endif  // LRB_ADBLOCK_DOMAIN_RESOLVER_H_
