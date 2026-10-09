// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/bookmarks.h"

#include <algorithm>
#include <set>

namespace lrb::coordinator {

namespace {

char Lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), Lower);
  return out;
}

bool IsWordStart(std::string_view text, size_t at) {
  if (at == 0) {
    return true;
  }
  const unsigned char before = static_cast<unsigned char>(text[at - 1]);
  return before < 0x80 && !((before >= 'a' && before <= 'z') ||
                            (before >= '0' && before <= '9'));
}

// 0: the address starts with `text`; 1: a title word does; 2: contained;
// -1: no match. `text` and `title` lowercased.
int Rank(std::string_view text, std::string_view address,
         std::string_view title) {
  if (address.starts_with(text)) {
    return 0;
  }
  for (size_t at = title.find(text); at != std::string_view::npos;
       at = title.find(text, at + 1)) {
    if (IsWordStart(title, at)) {
      return 1;
    }
  }
  // Inside a word only from 3 characters: one letter is in most of them.
  if (text.size() >= 3 && (address.find(text) != std::string_view::npos ||
                           title.find(text) != std::string_view::npos)) {
    return 2;
  }
  return -1;
}

}  // namespace

std::vector<Bookmark> ParseBookmarks(std::string_view file) {
  std::vector<Bookmark> bookmarks;
  while (!file.empty() && bookmarks.size() < kMaxBookmarks) {
    const size_t end = std::min(file.find('\n'), file.size());
    const std::string_view line = file.substr(0, end);
    file = end < file.size() ? file.substr(end + 1) : std::string_view();
    const size_t tab = line.find('\t');
    if (tab == 0 || line.empty()) {
      continue;
    }
    Bookmark bookmark;
    bookmark.url = std::string(line.substr(0, tab));
    if (bookmark.url.size() > kMaxBookmarkUrl) {
      continue;
    }
    if (tab != std::string_view::npos) {
      bookmark.title = CleanTitle(line.substr(tab + 1));
    }
    bookmarks.push_back(std::move(bookmark));
  }
  return bookmarks;
}

std::string SerializeBookmarks(const std::vector<Bookmark>& bookmarks) {
  std::string file;
  for (const Bookmark& bookmark : bookmarks) {
    file += bookmark.url;
    file += '\t';
    file += CleanTitle(bookmark.title);
    file += '\n';
  }
  return file;
}

std::string CleanTitle(std::string_view title) {
  std::string out;
  for (char c : title) {
    out += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? ' ' : c;
  }
  const size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) {
    return std::string();
  }
  out = out.substr(first, out.find_last_not_of(' ') - first + 1);
  constexpr size_t kMax = 300;
  if (out.size() > kMax) {
    size_t cut = kMax;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) {
      --cut;  // not inside a character
    }
    out.resize(cut);
  }
  return out;
}

std::string EncodeText(std::string_view text) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  for (char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || c == '.' || c == '_' || c == '~' ||
        c == '-' || c == '/') {
      out += c;
    } else {
      out += '%';
      out += kHex[byte >> 4];
      out += kHex[byte & 15];
    }
  }
  return out;
}

std::string DecodeText(std::string_view text) {
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    c = Lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() &&
        value(text[i + 1]) >= 0 && value(text[i + 2]) >= 0) {
      out += static_cast<char>(value(text[i + 1]) * 16 + value(text[i + 2]));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string_view DisplayAddress(std::string_view url) {
  for (std::string_view scheme : {"https://", "http://"}) {
    if (url.starts_with(scheme)) {
      url.remove_prefix(scheme.size());
      break;
    }
  }
  if (url.starts_with("www.")) {
    url.remove_prefix(4);
  }
  return url;
}

std::vector<Suggestion> Suggest(std::string_view text,
                                const std::vector<Bookmark>& bookmarks,
                                const std::vector<std::string>& sites,
                                size_t max) {
  std::string typed = LowerAscii(text);
  typed.erase(0, typed.find_first_not_of(' '));
  typed.erase(typed.find_last_not_of(' ') + 1);
  typed = std::string(DisplayAddress(typed));
  if (typed.empty() || max == 0) {
    return {};
  }
  // Candidates by rank, in order: bookmarks first (chosen by the user),
  // then sites, most recent first.
  std::vector<std::pair<int, Suggestion>> found;
  std::set<std::string> urls;
  for (const Bookmark& bookmark : bookmarks) {
    const int rank = Rank(typed, LowerAscii(DisplayAddress(bookmark.url)),
                          LowerAscii(bookmark.title));
    if (rank >= 0 && urls.insert(bookmark.url).second) {
      found.push_back({rank, {bookmark.url, bookmark.title, true}});
    }
  }
  for (const std::string& site : sites) {
    const std::string url = "https://" + site + "/";
    // Sites match by their name only: "e" in the middle of every name
    // would offer them all.
    if (site.starts_with(typed) && urls.insert(url).second) {
      found.push_back({0, {url, std::string(), false}});
    }
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<Suggestion> suggestions;
  for (auto& [rank, suggestion] : found) {
    if (suggestions.size() == max) {
      break;
    }
    suggestions.push_back(std::move(suggestion));
  }
  return suggestions;
}

}  // namespace lrb::coordinator
