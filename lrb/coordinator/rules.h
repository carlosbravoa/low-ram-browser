// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COORDINATOR_RULES_H_
#define LRB_COORDINATOR_RULES_H_

#include <optional>
#include <string>
#include <string_view>

// What the coordinator accepts from browser instances. Instances run web
// content and may be compromised, so nothing they send is trusted: a site
// becomes a directory name and a URL is handed to another site's window.

namespace lrb::coordinator {

// A site name the coordinator will use as a profile directory: lowercase
// letters, digits, '-' and '.', 1-253 chars, no leading/trailing '.', no
// empty labels (so never "." or ".."), and IPv4 literals.
bool IsValidSite(std::string_view site);

// An http(s) URL with no whitespace or control characters, at most 8 KiB.
bool IsAcceptableUrl(std::string_view url);

// The lowercased host of an http(s) URL, without userinfo or port; empty if
// there is none. IPv6 literals are returned with their brackets.
std::string HostOf(std::string_view url);

// Whether `url` belongs to `site`: its host is the site or a subdomain of
// it. Only such URLs may be sent to that site's window, so a compromised
// instance can't make another site's window load something else.
bool UrlBelongsToSite(std::string_view url, std::string_view site);

// A window's place, as "x,y,w,h": its top-left corner on the screen and the
// size of its page area. Used to open a site in the window it replaces.
struct WindowBounds {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Parses "x,y,w,h": integers, |x| and |y| at most 100000, width and height
// 100-20000.
std::optional<WindowBounds> ParseBounds(std::string_view text);
std::string FormatBounds(const WindowBounds& bounds);

}  // namespace lrb::coordinator

#endif  // LRB_COORDINATOR_RULES_H_
