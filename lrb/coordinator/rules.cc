// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/rules.h"

#include <ctype.h>

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

std::string ExternalAppScheme(std::string_view url) {
  constexpr size_t kMaxExternalUrlLength = 4096;
  // The browser's own schemes, and ones that read or run local things.
  constexpr std::array<std::string_view, 20> kNotForApps = {
      "http",       "https",       "ws",
      "wss",        "ftp",         "file",
      "data",       "javascript",  "vbscript",
      "blob",       "filesystem",  "about",
      "chrome",     "chrome-untrusted", "chrome-extension",
      "devtools",   "view-source", "content",
      "intent",     "isolated-app"};
  if (url.size() > kMaxExternalUrlLength) {
    return std::string();
  }
  const size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon > 32 ||
      colon + 1 == url.size()) {
    return std::string();
  }
  std::string scheme;
  for (size_t i = 0; i < colon; ++i) {
    const auto c = static_cast<unsigned char>(url[i]);
    const bool letter = isalpha(c);
    if (!letter && (i == 0 || !(isdigit(c) || c == '+' || c == '-' ||
                                c == '.'))) {
      return std::string();
    }
    scheme += static_cast<char>(tolower(c));
  }
  for (std::string_view refused : kNotForApps) {
    if (scheme == refused) {
      return std::string();
    }
  }
  for (char c : url) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) {
      return std::string();
    }
  }
  return scheme;
}

bool IsExternalAppUrl(std::string_view url) {
  return !ExternalAppScheme(url).empty();
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

std::optional<bool> JsonBool(std::string_view json, std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  size_t i = json.find(quoted);
  if (i == std::string_view::npos) {
    return std::nullopt;
  }
  i += quoted.size();
  auto skip_space = [&] {
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
                               json[i] == '\n' || json[i] == '\r')) {
      ++i;
    }
  };
  skip_space();
  if (i >= json.size() || json[i] != ':') {
    return std::nullopt;
  }
  ++i;
  skip_space();
  const std::string_view rest = json.substr(i);
  // The literal must end there: "trueish" or "falsey" isn't one.
  auto literal = [&rest](std::string_view word) {
    return rest.starts_with(word) &&
           (rest.size() == word.size() ||
            std::string_view(" \t\r\n,}").find(rest[word.size()]) !=
                std::string_view::npos);
  };
  if (literal("true")) {
    return true;
  }
  if (literal("false")) {
    return false;
  }
  return std::nullopt;
}

std::optional<std::string> JsonString(std::string_view json,
                                      std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  size_t i = json.find(quoted);
  if (i == std::string_view::npos) {
    return std::nullopt;
  }
  i += quoted.size();
  auto skip_space = [&] {
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
                               json[i] == '\n' || json[i] == '\r')) {
      ++i;
    }
  };
  skip_space();
  if (i >= json.size() || json[i] != ':') {
    return std::nullopt;
  }
  ++i;
  skip_space();
  if (i >= json.size() || json[i] != '"') {
    return std::nullopt;
  }
  std::string value;
  for (++i; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '"') {
      return value;
    }
    if (static_cast<unsigned char>(c) < 0x20) {
      return std::nullopt;  // control characters must be escaped
    }
    if (c != '\\') {
      value += c;
      continue;
    }
    if (++i >= json.size()) {
      return std::nullopt;
    }
    switch (json[i]) {
      case '"':
      case '\\':
      case '/':
        value += json[i];
        break;
      case 'b':
        value += '\b';
        break;
      case 'f':
        value += '\f';
        break;
      case 'n':
        value += '\n';
        break;
      case 'r':
        value += '\r';
        break;
      case 't':
        value += '\t';
        break;
      case 'u': {
        if (i + 4 >= json.size()) {
          return std::nullopt;
        }
        int code = 0;
        for (size_t k = 1; k <= 4; ++k) {
          const char h = json[i + k];
          const int digit = h >= '0' && h <= '9'   ? h - '0'
                            : h >= 'a' && h <= 'f' ? h - 'a' + 10
                            : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                   : -1;
          if (digit < 0) {
            return std::nullopt;
          }
          code = code * 16 + digit;
        }
        if (code >= 0x80) {
          return std::nullopt;  // not needed for lrb's plain values
        }
        value += static_cast<char>(code);
        i += 4;
        break;
      }
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;  // unterminated
}

}  // namespace lrb::coordinator
