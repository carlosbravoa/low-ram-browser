// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/rules.h"

#include <array>

namespace lrb::coordinator {

namespace {

constexpr size_t kMaxUrlLength = 8192;

bool IsSiteChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.';
}

char ToLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

bool IsValidSite(std::string_view site) {
  if (site.empty() || site.size() > 253 || site.front() == '.' ||
      site.back() == '.') {
    return false;
  }
  char previous = 0;
  for (char c : site) {
    if (!IsSiteChar(c) || (c == '.' && previous == '.')) {
      return false;
    }
    previous = c;
  }
  return true;
}

bool IsAcceptableUrl(std::string_view url) {
  if (url.size() > kMaxUrlLength ||
      !(url.starts_with("http://") || url.starts_with("https://"))) {
    return false;
  }
  for (char c : url) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) {
      return false;
    }
  }
  return !HostOf(url).empty();
}

std::string HostOf(std::string_view url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return std::string();
  }
  std::string_view rest = url.substr(scheme_end + 3);
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
    rest = rest.substr(at + 1);  // userinfo
  }
  if (rest.starts_with("[")) {
    const size_t close = rest.find(']');
    return close == std::string_view::npos ? std::string()
                                           : std::string(rest.substr(0, close + 1));
  }
  rest = rest.substr(0, rest.find(':'));  // port
  std::string host;
  host.reserve(rest.size());
  for (char c : rest) {
    host.push_back(ToLower(c));
  }
  if (host.ends_with(".")) {
    host.pop_back();  // "example.com." is example.com
  }
  return host;
}

bool UrlBelongsToSite(std::string_view url, std::string_view site) {
  if (!IsAcceptableUrl(url) || !IsValidSite(site)) {
    return false;
  }
  const std::string host = HostOf(url);
  return host == site ||
         (host.size() > site.size() && host.ends_with(site) &&
          host[host.size() - site.size() - 1] == '.');
}

std::optional<WindowBounds> ParseBounds(std::string_view text) {
  // Four comma-separated integers, each optionally negative, at most six
  // digits (the range check below is tighter).
  std::array<int, 4> values = {};
  size_t field = 0;
  size_t digits = 0;
  bool negative = false;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : ',';
    if (c == '-' && digits == 0 && !negative) {
      negative = true;
    } else if (c >= '0' && c <= '9' && digits < 6 && field < values.size()) {
      values[field] = values[field] * 10 + (c - '0');
      ++digits;
    } else if (c == ',' && digits > 0 && field < values.size()) {
      if (negative) {
        values[field] = -values[field];
      }
      ++field;
      digits = 0;
      negative = false;
    } else {
      return std::nullopt;
    }
  }
  if (field != values.size()) {
    return std::nullopt;
  }
  const WindowBounds bounds = {values[0], values[1], values[2], values[3]};
  auto within = [](int v, int low, int high) { return v >= low && v <= high; };
  if (!within(bounds.x, -100000, 100000) ||
      !within(bounds.y, -100000, 100000) ||
      !within(bounds.width, 100, 20000) ||
      !within(bounds.height, 100, 20000)) {
    return std::nullopt;
  }
  return bounds;
}

std::string FormatBounds(const WindowBounds& bounds) {
  return std::to_string(bounds.x) + "," + std::to_string(bounds.y) + "," +
         std::to_string(bounds.width) + "," + std::to_string(bounds.height);
}

}  // namespace lrb::coordinator
