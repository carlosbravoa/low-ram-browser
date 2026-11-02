// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/history.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/values_util.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"

namespace lrb {

namespace {

const char kUserDataKey[] = "lrb-history";
constexpr char kFileName[] = "lrb-history.json";
constexpr base::TimeDelta kSaveDelay = base::Seconds(2);
// Kept per page at most: a title is for recognising a page in a list.
constexpr size_t kMaxTitle = 300;
// Addresses longer than this (tracking parameters, data in the URL) aren't
// worth keeping 3000 of.
constexpr size_t kMaxUrl = 2048;

std::string Clean(std::u16string_view title) {
  std::string out = base::UTF16ToUTF8(title.substr(0, kMaxTitle));
  for (char& c : out) {
    if (static_cast<unsigned char>(c) < 0x20) {
      c = ' ';
    }
  }
  return std::string(base::TrimWhitespaceASCII(out, base::TRIM_ALL));
}

// `url` without its scheme and "www.", as the coordinator matches
// bookmarks (DisplayAddress in lrb/coordinator/bookmarks.h).
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

bool IsWordStart(std::string_view text, size_t at) {
  if (at == 0) {
    return true;
  }
  const unsigned char before = static_cast<unsigned char>(text[at - 1]);
  return before < 0x80 && !((before >= 'a' && before <= 'z') ||
                            (before >= '0' && before <= '9'));
}

// 0: the address starts with `text`; 1: a title word does; 2: contained
// (from 3 characters); -1: no match. All lowercased. The coordinator's
// ranking of bookmarks, so both lists read the same.
int Rank(std::string_view text,
         std::string_view address,
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
  if (text.size() >= 3 && (address.find(text) != std::string_view::npos ||
                           title.find(text) != std::string_view::npos)) {
    return 2;
  }
  return -1;
}

}  // namespace

History::History(const base::FilePath& file)
    : file_(file),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath file) {
            std::vector<Entry> entries;
            std::string json;
            if (!base::ReadFileToString(file, &json)) {
              return entries;
            }
            std::optional<base::Value> value =
                base::JSONReader::Read(json, base::JSON_PARSE_RFC);
            if (!value || !value->is_list()) {
              LOG(WARNING) << "lrb: unreadable " << file;
              return entries;
            }
            for (const base::Value& item : value->GetList()) {
              const base::DictValue* page = item.GetIfDict();
              const std::string* url = page ? page->FindString("u") : nullptr;
              if (!url || url->size() > kMaxUrl ||
                  !GURL(*url).SchemeIsHTTPOrHTTPS()) {
                continue;
              }
              Entry entry;
              entry.url = *url;
              if (const std::string* title = page->FindString("t")) {
                entry.title = title->substr(0, kMaxTitle * 3);
              }
              if (const base::Value* visited = page->Find("v")) {
                entry.visited = base::ValueToTime(*visited).value_or(
                    base::Time());
              }
              entries.push_back(std::move(entry));
              if (entries.size() == kMax) {
                break;
              }
            }
            return entries;
          },
          file_),
      base::BindOnce(&History::OnLoaded, weak_factory_.GetWeakPtr()));
}

History::~History() {
  // The profile is going (the browser quits): a pending save is written
  // now; the sequence blocks shutdown until it is.
  if (save_timer_.IsRunning()) {
    save_timer_.Stop();
    Save();
  }
}

// static
History* History::For(content::BrowserContext* context) {
  if (!context || context->IsOffTheRecord() || context->GetPath().empty()) {
    return nullptr;
  }
  auto* history = static_cast<History*>(context->GetUserData(kUserDataKey));
  if (!history) {
    auto created =
        base::WrapUnique(new History(context->GetPath().Append(kFileName)));
    history = created.get();
    context->SetUserData(kUserDataKey, std::move(created));
  }
  return history;
}

void History::OnLoaded(std::vector<Entry> loaded) {
  // Pages visited while the file was read are newer: they stay in front.
  for (Entry& entry : loaded) {
    if (Find(entry.url) == entries_.end()) {
      entries_.push_back(std::move(entry));
    }
  }
  if (entries_.size() > kMax) {
    entries_.resize(kMax);
  }
}

std::vector<History::Entry>::iterator History::Find(std::string_view url) {
  return std::find_if(entries_.begin(), entries_.end(),
                      [url](const Entry& entry) { return entry.url == url; });
}

void History::Add(const GURL& url, const std::u16string& title) {
  if (!url.SchemeIsHTTPOrHTTPS() || url.spec().size() > kMaxUrl) {
    return;
  }
  Entry entry;
  entry.url = url.spec();
  entry.visited = base::Time::Now();
  auto found = Find(entry.url);
  if (found != entries_.end()) {
    entry.title = std::move(found->title);
    entries_.erase(found);
  }
  if (std::string cleaned = Clean(title); !cleaned.empty()) {
    entry.title = std::move(cleaned);
  }
  entries_.insert(entries_.begin(), std::move(entry));
  if (entries_.size() > kMax) {
    entries_.pop_back();
  }
  Changed();
}

void History::SetTitle(const GURL& url, const std::u16string& title) {
  std::string cleaned = Clean(title);
  auto found = Find(url.spec());
  if (cleaned.empty() || found == entries_.end() || found->title == cleaned) {
    return;
  }
  found->title = std::move(cleaned);
  Changed();
}

std::vector<History::Page> History::Recent(size_t max) const {
  std::vector<Page> pages;
  for (const Entry& entry : entries_) {
    if (pages.size() == max) {
      break;
    }
    pages.push_back(
        {GURL(entry.url), base::UTF8ToUTF16(entry.title), entry.visited});
  }
  return pages;
}

std::vector<History::Page> History::Match(std::u16string_view text,
                                          size_t max) const {
  std::string typed = base::ToLowerASCII(
      base::TrimWhitespaceASCII(base::UTF16ToUTF8(text), base::TRIM_ALL));
  typed = std::string(DisplayAddress(typed));
  if (typed.empty() || max == 0) {
    return {};
  }
  // The best `max` of each rank, newest first (entries_ is in that order).
  std::array<std::vector<const Entry*>, 3> by_rank;
  for (const Entry& entry : entries_) {
    const int rank =
        Rank(typed, base::ToLowerASCII(DisplayAddress(entry.url)),
             base::ToLowerASCII(entry.title));
    if (rank >= 0 && by_rank.at(rank).size() < max) {
      by_rank.at(rank).push_back(&entry);
    }
    if (by_rank[0].size() == max) {
      break;  // nothing can beat these
    }
  }
  std::vector<Page> pages;
  for (const std::vector<const Entry*>& entries : by_rank) {
    for (const Entry* entry : entries) {
      if (pages.size() == max) {
        return pages;
      }
      pages.push_back(
          {GURL(entry->url), base::UTF8ToUTF16(entry->title), entry->visited});
    }
  }
  return pages;
}

void History::Changed() {
  save_timer_.Start(FROM_HERE, kSaveDelay,
                    base::BindOnce(&History::Save, base::Unretained(this)));
}

void History::Save() {
  base::ListValue list;
  for (const Entry& entry : entries_) {
    base::DictValue page;
    page.Set("u", entry.url);
    if (!entry.title.empty()) {
      page.Set("t", entry.title);
    }
    page.Set("v", base::TimeToValue(entry.visited));
    list.Append(std::move(page));
  }
  std::optional<std::string> json = base::WriteJson(list);
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath file, std::string json) {
            if (!base::ImportantFileWriter::WriteFileAtomically(file, json)) {
              LOG(ERROR) << "lrb: could not save " << file;
            }
          },
          file_, json.value_or("[]")));
}

HistoryRecorder::HistoryRecorder(content::WebContents* contents)
    : content::WebContentsObserver(contents) {
  // Created with the first page: the file is read while it loads.
  History::For(contents->GetBrowserContext());
}

HistoryRecorder::~HistoryRecorder() = default;

void HistoryRecorder::DidFinishNavigation(content::NavigationHandle* handle) {
  if (!handle->HasCommitted() || !handle->IsInPrimaryMainFrame() ||
      handle->IsErrorPage() || !handle->GetURL().SchemeIsHTTPOrHTTPS()) {
    return;  // a discarded page's about:blank among them
  }
  History* history = History::For(web_contents()->GetBrowserContext());
  if (!history) {
    return;
  }
  content::NavigationEntry* entry =
      web_contents()->GetController().GetLastCommittedEntry();
  history->Add(handle->GetURL(),
               entry ? entry->GetTitle() : std::u16string());
}

void HistoryRecorder::TitleWasSet(content::NavigationEntry* entry) {
  if (!entry ||
      entry != web_contents()->GetController().GetLastCommittedEntry() ||
      !entry->GetURL().SchemeIsHTTPOrHTTPS()) {
    return;
  }
  if (History* history = History::For(web_contents()->GetBrowserContext())) {
    history->SetTitle(entry->GetURL(), entry->GetTitle());
  }
}

}  // namespace lrb
