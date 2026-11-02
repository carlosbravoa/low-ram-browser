// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_HISTORY_H_
#define LRB_BROWSER_HISTORY_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}

namespace lrb {

// The pages visited in one profile, newest first, one entry per address,
// for the address bar's suggestions and the menu's History. Kept in the
// profile (lrb-history.json), so in lean mode a site's history lives and
// goes with its own profile, and nothing crosses sites. At most kMax pages:
// the oldest go.
//
// Attached to the BrowserContext (user data): created on first use, which
// starts reading the file off the UI thread; pages visited before it is
// read are kept and merged in. Saved 2 s after the last change (one write
// for a burst of navigations), on a sequence that finishes before exit.
// Addresses and titles are kept as UTF-8 strings, not GURLs: a parsed GURL
// costs about twice the memory and 3000 of them are kept.
class History : public base::SupportsUserData::Data {
 public:
  static constexpr size_t kMax = 3000;

  struct Page {
    GURL url;
    std::u16string title;
    base::Time visited;
  };

  ~History() override;
  History(const History&) = delete;
  History& operator=(const History&) = delete;

  // `context`'s history, created on first use. Null for an off-the-record
  // profile: nothing is recorded there.
  static History* For(content::BrowserContext* context);

  // A visit to `url` (http(s)): it moves to the front, with `title` if
  // given (else the one it had).
  void Add(const GURL& url, const std::u16string& title);
  // The page's title changed (after it committed).
  void SetTitle(const GURL& url, const std::u16string& title);

  // The `max` most recent pages.
  std::vector<Page> Recent(size_t max) const;

  // What the address bar offers for `text`, at most `max`, as the
  // coordinator ranks bookmarks (lrb/coordinator/bookmarks.h): pages whose
  // address (without scheme and "www.") starts with it, then pages with a
  // title word starting with it, then ones containing it anywhere (from 3
  // characters). Most recent first within each rank. Case-insensitive
  // (ASCII).
  std::vector<Page> Match(std::u16string_view text, size_t max) const;

 private:
  struct Entry {
    std::string url;
    std::string title;
    base::Time visited;
  };

  explicit History(const base::FilePath& file);

  void OnLoaded(std::vector<Entry> loaded);
  // The entry for `url`, or entries_.end().
  std::vector<Entry>::iterator Find(std::string_view url);
  void Changed();
  void Save();

  const base::FilePath file_;
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  std::vector<Entry> entries_;  // newest first
  base::OneShotTimer save_timer_;
  base::WeakPtrFactory<History> weak_factory_{this};
};

// Records a page's visits in its profile's History: committed http(s) pages
// of the primary main frame (not error pages), and their titles as they
// come. One per Shell.
class HistoryRecorder : public content::WebContentsObserver {
 public:
  explicit HistoryRecorder(content::WebContents* contents);
  HistoryRecorder(const HistoryRecorder&) = delete;
  HistoryRecorder& operator=(const HistoryRecorder&) = delete;
  ~HistoryRecorder() override;

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;
  void TitleWasSet(content::NavigationEntry* entry) override;
};

}  // namespace lrb

#endif  // LRB_BROWSER_HISTORY_H_
