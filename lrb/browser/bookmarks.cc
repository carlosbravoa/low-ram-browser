// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/bookmarks.h"

#include <map>
#include <string_view>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/coordinator/bookmarks.h"

namespace lrb {

namespace {

// The coordinator drops an instance whose line is over 9000 bytes, and
// keeps no bookmark longer than this.
constexpr size_t kMaxUrl = coordinator::kMaxBookmarkUrl;
constexpr size_t kMaxTitle = 150;  // characters; encoded up to ~6x
constexpr size_t kMaxTyped = 200;

// Requests waiting for their answer, by id.
struct Pending {
  std::vector<Suggestion> found;
  SuggestionsCallback done;
  base::OnceCallback<void(bool)> found_callback;
};

std::map<uint64_t, Pending>& Requests() {
  static base::NoDestructor<std::map<uint64_t, Pending>> requests;
  return *requests;
}

LrbContentBrowserClient* Coordinated() {
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  return client && client->has_coordinator() ? client : nullptr;
}

uint64_t NextId() {
  static uint64_t next = 0;
  return ++next;
}

// Splits "<a> <b> ..." into its first `n` words and the rest (the last
// element).
std::vector<std::string_view> Words(std::string_view text, size_t n) {
  std::vector<std::string_view> words;
  while (words.size() + 1 < n) {
    const size_t space = text.find(' ');
    if (space == std::string_view::npos) {
      break;
    }
    words.push_back(text.substr(0, space));
    text = text.substr(space + 1);
  }
  words.push_back(text);
  return words;
}

std::string Title(const std::u16string& title) {
  return base::UTF16ToUTF8(title.substr(0, kMaxTitle));
}

}  // namespace

void Suggest(const std::u16string& text, SuggestionsCallback done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client || text.empty() || text.size() > kMaxTyped) {
    std::move(done).Run({});
    return;
  }
  const uint64_t id = NextId();
  Requests()[id].done = std::move(done);
  client->SendToCoordinator(
      "suggest " + base::NumberToString(id) + " " +
      coordinator::EncodeText(base::UTF16ToUTF8(text)));
}

void ListBookmarks(SuggestionsCallback done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client) {
    std::move(done).Run({});
    return;
  }
  const uint64_t id = NextId();
  Requests()[id].done = std::move(done);
  client->SendToCoordinator("bookmarks " + base::NumberToString(id));
}

void IsBookmarked(const GURL& url, base::OnceCallback<void(bool)> done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client || !url.SchemeIsHTTPOrHTTPS() || url.spec().size() > kMaxUrl) {
    std::move(done).Run(false);
    return;
  }
  const uint64_t id = NextId();
  Requests()[id].found_callback = std::move(done);
  client->SendToCoordinator("is-bookmarked " + base::NumberToString(id) + " " +
                            url.spec());
}

void AddBookmark(const GURL& url, const std::u16string& title) {
  if (LrbContentBrowserClient* client = Coordinated();
      client && url.SchemeIsHTTPOrHTTPS() && url.spec().size() <= kMaxUrl) {
    client->SendToCoordinator("bookmark-add " + url.spec() + " " +
                              coordinator::EncodeText(Title(title)));
  }
}

void RemoveBookmark(const GURL& url) {
  if (LrbContentBrowserClient* client = Coordinated();
      client && url.SchemeIsHTTPOrHTTPS() && url.spec().size() <= kMaxUrl) {
    client->SendToCoordinator("bookmark-remove " + url.spec());
  }
}

bool OnBookmarksLine(const std::string& line) {
  const size_t space = line.find(' ');
  const std::string_view command = std::string_view(line).substr(0, space);
  if (command != "suggestion" && command != "suggestions-done" &&
      command != "bookmark-item" && command != "bookmarks-done" &&
      command != "is-bookmarked") {
    return false;
  }
  const std::string_view args =
      space == std::string::npos ? "" : std::string_view(line).substr(space + 1);
  const std::vector<std::string_view> words =
      Words(args, command == "suggestion" ? 4 : 3);
  uint64_t id = 0;
  auto request = base::StringToUint64(words[0], &id)
                     ? Requests().find(id)
                     : Requests().end();
  if (request == Requests().end()) {
    return true;
  }
  Pending& pending = request->second;
  if (command == "suggestion" && words.size() == 4) {
    // <id> <b|s> <url> <title>
    Suggestion suggestion;
    suggestion.bookmark = words[1] == "b";
    suggestion.url = GURL(words[2]);
    suggestion.title =
        base::UTF8ToUTF16(coordinator::DecodeText(words[3]));
    if (suggestion.url.SchemeIsHTTPOrHTTPS()) {
      pending.found.push_back(std::move(suggestion));
    }
  } else if (command == "bookmark-item" && words.size() == 3) {
    // <id> <url> <title>
    Suggestion bookmark;
    bookmark.bookmark = true;
    bookmark.url = GURL(words[1]);
    bookmark.title = base::UTF8ToUTF16(coordinator::DecodeText(words[2]));
    if (bookmark.url.SchemeIsHTTPOrHTTPS()) {
      pending.found.push_back(std::move(bookmark));
    }
  } else if (command == "is-bookmarked") {
    auto done = std::move(pending.found_callback);
    Requests().erase(request);
    if (done) {
      std::move(done).Run(words.size() > 1 && words[1] == "1");
    }
  } else if (command == "suggestions-done" || command == "bookmarks-done") {
    Pending finished = std::move(pending);
    Requests().erase(request);
    if (finished.done) {
      std::move(finished.done).Run(std::move(finished.found));
    }
  }
  return true;
}

}  // namespace lrb
