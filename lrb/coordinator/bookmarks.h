// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COORDINATOR_BOOKMARKS_H_
#define LRB_COORDINATOR_BOOKMARKS_H_

#include <string>
#include <string_view>
#include <vector>

// Bookmarks and address-bar suggestions, kept by the coordinator (decided
// 2026-10-08): they span sites, and a site's instance can't read other
// sites' profiles. Instances ask for them over the coordinator's socket:
//
//   instance -> coordinator           coordinator -> instance
//   suggest <id> <text>               suggestion <id> <b|s> <url> <title>
//                                     ... suggestions-done <id>
//   bookmarks <id>                    bookmark-item <id> <url> <title>
//                                     ... bookmarks-done <id>
//   is-bookmarked <id> <url>          is-bookmarked <id> 0|1
//   bookmark-add <url> <title>
//   bookmark-remove <url>
//
// <text> and <title> are percent-encoded (EncodeText), <id> the instance's.
// A compromised instance can ask too: it learns which sites the user visits
// and bookmarked (accepted, 2026-10-08), but adds and removes bookmarks of
// its own site only, so it can't plant one under another site's address.
//
// Kept as <profiles-dir>/.bookmarks, a line per bookmark, "<url>\t<title>".
// Visited sites are the profiles' directories, most recently changed first.
namespace lrb::coordinator {

struct Bookmark {
  std::string url;
  std::string title;
};

// Bookmarks kept at most; adding to a full list is refused.
inline constexpr size_t kMaxBookmarks = 1000;
// A bookmark's URL at most: its reply line (with a title of at most 300
// bytes, encoded) must stay under the instances' 9000-byte line limit, or
// one site's bookmark would cut every other site off the coordinator.
inline constexpr size_t kMaxBookmarkUrl = 7000;
// The bookmarks file at most (it is read for every keystroke in an address
// bar and sent whole to every menu): adding beyond it is refused.
inline constexpr size_t kMaxBookmarksBytes = 512 * 1024;

std::vector<Bookmark> ParseBookmarks(std::string_view file);
std::string SerializeBookmarks(const std::vector<Bookmark>& bookmarks);

// A title as kept: control characters become spaces, surrounding spaces go,
// at most 300 bytes (cut at a UTF-8 character's start).
std::string CleanTitle(std::string_view title);

// Percent-encoding for text in a protocol line (no spaces, no newlines).
std::string EncodeText(std::string_view text);
std::string DecodeText(std::string_view text);

struct Suggestion {
  std::string url;
  std::string title;  // empty for a site
  bool bookmark = false;
};

// What the address bar offers for `text`, best first, at most `max`:
// bookmarks and visited `sites` (most recent first) whose address starts
// with it, then bookmarks with a title word starting with it, then ones
// containing it anywhere (from 3 characters typed). Case-insensitive
// (ASCII); "https://" and "www." typed are ignored. A site is offered as
// https://<site>/.
std::vector<Suggestion> Suggest(std::string_view text,
                                const std::vector<Bookmark>& bookmarks,
                                const std::vector<std::string>& sites,
                                size_t max);

// `url` without its scheme and "www.": how the address bar matches it, and
// what inline completion completes to.
std::string_view DisplayAddress(std::string_view url);

}  // namespace lrb::coordinator

#endif  // LRB_COORDINATOR_BOOKMARKS_H_
