// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_BOOKMARKS_H_
#define LRB_BROWSER_BOOKMARKS_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "url/gurl.h"

namespace lrb {

// Bookmarks and address-bar suggestions, asked of the coordinator, which
// keeps them for all sites (lrb/coordinator/bookmarks.h). Nothing is kept
// in this process. Without a coordinator there are none.
struct Suggestion {
  GURL url;
  std::u16string title;  // empty for a visited site
  bool bookmark = false;
};
using SuggestionsCallback =
    base::OnceCallback<void(std::vector<Suggestion>)>;

// What the address bar offers for `text` (best first).
void Suggest(const std::u16string& text, SuggestionsCallback done);
// All bookmarks, in the order added.
void ListBookmarks(SuggestionsCallback done);
void IsBookmarked(const GURL& url, base::OnceCallback<void(bool)> done);
// Only pages of this instance's site (the coordinator refuses others).
void AddBookmark(const GURL& url, const std::u16string& title);
void RemoveBookmark(const GURL& url);

// A line from the coordinator: true if it was an answer to one of these.
bool OnBookmarksLine(const std::string& line);

}  // namespace lrb

#endif  // LRB_BROWSER_BOOKMARKS_H_
