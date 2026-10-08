// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/saved_windows.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_entry_restore_context.h"
#include "content/public/browser/restore_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/site_launcher.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "third_party/blink/public/common/page_state/page_state.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace lrb {

namespace {

constexpr char kFileName[] = "lrb-saved-windows.json";
constexpr char kLeftFileName[] = "lrb-left-window.json";

base::FilePath SavedPath(content::BrowserContext* browser_context) {
  return browser_context->GetPath().Append(kFileName);
}

base::FilePath LeftPath(content::BrowserContext* browser_context) {
  return browser_context->GetPath().Append(kLeftFileName);
}

bool g_keep_session = false;

// Every write and delete of saved windows, in order: a late write can't
// bring back a session just deleted. Finished before the process exits.
scoped_refptr<base::SequencedTaskRunner> FileRunner() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *runner;
}

base::OneShotTimer& SessionTimer() {
  static base::NoDestructor<base::OneShotTimer> timer;
  return *timer;
}

// A site's own windows (not the resolver's, which only finds the site).
bool HasSession(content::BrowserContext* browser_context) {
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  return !g_keep_session && !browser_context->IsOffTheRecord() && client &&
         !client->site().empty();
}

// One window's history, without about:blank entries (a discard's).
base::DictValue SerializeWindow(Shell* shell) {
  content::NavigationController& controller =
      shell->web_contents()->GetController();
  base::ListValue entries;
  int current = -1;
  for (int i = 0; i < controller.GetEntryCount(); ++i) {
    content::NavigationEntry* entry = controller.GetEntryAtIndex(i);
    if (entry->GetURL().IsAboutBlank()) {
      continue;
    }
    base::DictValue saved;
    saved.Set("url", entry->GetURL().spec());
    saved.Set("title", entry->GetTitle());
    saved.Set("state", base::Base64Encode(entry->GetPageState().ToEncodedData()));
    entries.Append(std::move(saved));
    if (i <= controller.GetCurrentEntryIndex()) {
      current = static_cast<int>(entries.size()) - 1;
    }
  }
  base::DictValue window;
  window.Set("current", current);
  window.Set("entries", std::move(entries));
  // Where Back goes from its first page, if it was opened from another site.
  if (const GURL back = LrbPlatformDelegate::BackUrlOf(shell);
      back.is_valid()) {
    window.Set("back", back.spec());
  }
  return window;
}

// The current entry's URL and title of a saved tab's history.
std::pair<GURL, std::u16string> CurrentOf(const base::DictValue& history) {
  const base::ListValue* entries = history.FindList("entries");
  const std::optional<int> current = history.FindInt("current");
  if (!entries || !current || *current < 0 ||
      *current >= static_cast<int>(entries->size()) ||
      !(*entries)[*current].is_dict()) {
    return {};
  }
  const base::DictValue& entry = (*entries)[*current].GetDict();
  const std::string* url = entry.FindString("url");
  const std::string* title = entry.FindString("title");
  return {url ? GURL(*url) : GURL(),
          title ? base::UTF8ToUTF16(*title) : std::u16string()};
}

// Loads `tab` into a new Shell: its saved history (the current entry
// loads), else its address. Where the Shell goes (a window, a tab) is the
// platform delegate's.
Shell* RestoreTab(content::BrowserContext* browser_context,
                           const LrbPlatformDelegate::TabState& tab) {
  LrbPlatformDelegate::SetNextWindowBackUrl(tab.back_url);
  const base::ListValue* saved_entries =
      tab.history ? tab.history->FindList("entries") : nullptr;
  const std::optional<int> current =
      tab.history ? tab.history->FindInt("current") : std::nullopt;
  if (!saved_entries || saved_entries->empty() || !current ||
      *current < 0 || *current >= static_cast<int>(saved_entries->size())) {
    return Shell::CreateNewWindow(
        browser_context, tab.url.is_valid() ? tab.url : GURL(url::kAboutBlankURL),
        nullptr, gfx::Size());
  }
  std::unique_ptr<content::NavigationEntryRestoreContext> restore_context =
      content::NavigationEntryRestoreContext::Create();
  std::vector<std::unique_ptr<content::NavigationEntry>> entries;
  for (const base::Value& value : *saved_entries) {
    const base::DictValue* saved = value.GetIfDict();
    const std::string* url = saved ? saved->FindString("url") : nullptr;
    if (!url) {
      return Shell::CreateNewWindow(browser_context, tab.url, nullptr,
                                             gfx::Size());
    }
    std::unique_ptr<content::NavigationEntry> entry =
        content::NavigationController::CreateNavigationEntry(
            GURL(*url), content::Referrer(), std::nullopt, std::nullopt,
            ui::PAGE_TRANSITION_RELOAD, /*is_renderer_initiated=*/false,
            std::string(), browser_context, nullptr);
    std::string state;
    if (const std::string* encoded = saved->FindString("state");
        encoded && base::Base64Decode(*encoded, &state) && !state.empty()) {
      entry->SetPageState(blink::PageState::CreateFromEncodedData(state),
                          restore_context.get());
    }
    if (const std::string* title = saved->FindString("title")) {
      entry->SetTitle(base::UTF8ToUTF16(*title));
    }
    entries.push_back(std::move(entry));
  }
  // An empty page, then its history; the current entry loads.
  Shell* shell = Shell::CreateNewWindow(
      browser_context, GURL(), nullptr, gfx::Size());
  content::NavigationController& controller =
      shell->web_contents()->GetController();
  controller.Restore(*current, content::RestoreType::kRestored, &entries);
  controller.LoadIfNecessary();
  return shell;
}

// A saved tab (one history, as SerializeWindow writes it).
LrbPlatformDelegate::TabState TabFromHistory(const base::DictValue& history) {
  LrbPlatformDelegate::TabState tab;
  std::tie(tab.url, tab.title) = CurrentOf(history);
  if (const std::string* back = history.FindString("back")) {
    tab.back_url = GURL(*back);
  }
  tab.history = history.Clone();
  return tab;
}

void Write(base::FilePath path, base::ListValue windows,
           base::OnceClosure done) {
  std::optional<std::string> json = base::WriteJson(windows);
  FileRunner()->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath path, std::string json) {
            if (!base::CreateDirectory(path.DirName()) ||
                !base::WriteFile(path, json)) {
              LOG(ERROR) << "lrb: could not save windows to " << path;
            }
          },
          path, json.value_or("[]")),
      std::move(done));
}

void OnSavedRead(content::BrowserContext* browser_context,
                 const GURL& startup_url,
                 bool resume,
                 std::optional<std::string> json) {
  std::optional<base::ListValue> windows;
  if (json) {
    windows = base::JSONReader::ReadList(*json, base::JSON_PARSE_RFC);
    if (!windows) {
      LOG(WARNING) << "lrb: unreadable saved windows";
    }
  }
  bool startup_url_shown = false;
  if (windows) {
    VLOG(1) << "lrb: restoring " << windows->size() << " saved window(s)";
    for (const base::Value& value : *windows) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& window = value.GetDict();
      // {"tabs": [history, ...], "active": n}; or one history (before tabs,
      // and a left window).
      std::vector<LrbPlatformDelegate::TabState> tabs;
      size_t active = 0;
      if (const base::ListValue* saved_tabs = window.FindList("tabs")) {
        for (const base::Value& tab : *saved_tabs) {
          if (tab.is_dict()) {
            tabs.push_back(TabFromHistory(tab.GetDict()));
          }
        }
        active = static_cast<size_t>(std::max(0, window.FindInt("active").value_or(0)));
      } else {
        tabs.push_back(TabFromHistory(window));
      }
      if (tabs.empty()) {
        continue;
      }
      active = std::min(active, tabs.size() - 1);
      startup_url_shown |= tabs[active].url == startup_url;
      LrbPlatformDelegate::OpenWindow(browser_context, std::move(tabs), active,
                                      base::BindRepeating(&RestoreTab));
    }
  }
  // Reopening a closed site usually asks for the page it was showing: one
  // window for it, not two (at 384 MB the duplicate load sank the instance).
  // Started on the last site: its windows as they were are the start.
  if (resume && !Shell::windows().empty()) {
    startup_url_shown = true;
  }
  if (!startup_url_shown || Shell::windows().empty()) {
    Shell::CreateNewWindow(browser_context, startup_url, nullptr,
                                    gfx::Size());
  }
}

}  // namespace

void SessionChanged(content::BrowserContext* browser_context) {
  if (!HasSession(browser_context)) {
    return;
  }
  SessionTimer().Start(
      FROM_HERE, base::Seconds(2),
      base::BindOnce(
          [](content::BrowserContext* browser_context) {
            if (HasSession(browser_context) && !Shell::windows().empty()) {
              SaveWindows(browser_context, base::DoNothing());
            }
          },
          base::Unretained(browser_context)));
}

void SessionClosedByUser(content::BrowserContext* browser_context) {
  SessionTimer().Stop();
  if (HasSession(browser_context)) {
    FileRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                       SavedPath(browser_context)));
  }
}

void KeepSession() {
  SessionTimer().Stop();
  g_keep_session = true;
}

base::DictValue SerializeTab(Shell* shell) {
  return SerializeWindow(shell);
}

Shell* LoadSavedTab(content::BrowserContext* browser_context,
                    const LrbPlatformDelegate::TabState& tab) {
  return RestoreTab(browser_context, tab);
}

void SaveWindows(content::BrowserContext* browser_context,
                 base::OnceClosure done) {
  // {"tabs": [history, ...], "active": n} per window. A tab not loaded keeps
  // the history it was restored with, or just its address.
  base::ListValue windows;
  for (const LrbPlatformDelegate::WindowState& window :
       LrbPlatformDelegate::Windows()) {
    base::ListValue tabs;
    for (const LrbPlatformDelegate::TabState& tab : window.tabs) {
      if (tab.shell) {
        tabs.Append(SerializeWindow(tab.shell));
      } else if (tab.history) {
        tabs.Append(tab.history->Clone());
      } else {
        base::DictValue entry;
        entry.Set("url", tab.url.spec());
        entry.Set("title", tab.title);
        base::ListValue entries;
        entries.Append(std::move(entry));
        base::DictValue history;
        history.Set("current", 0);
        history.Set("entries", std::move(entries));
        if (tab.back_url.is_valid()) {
          history.Set("back", tab.back_url.spec());
        }
        tabs.Append(std::move(history));
      }
    }
    base::DictValue saved;
    saved.Set("tabs", std::move(tabs));
    saved.Set("active", static_cast<int>(window.active));
    windows.Append(std::move(saved));
  }
  Write(SavedPath(browser_context), std::move(windows), std::move(done));
}

void SaveLeftWindow(Shell* shell,
                    const std::string& site,
                    base::OnceClosure done) {
  base::ListValue windows;
  windows.Append(SerializeWindow(shell));
  // Into the site's own profile, where its window coming back looks: not
  // necessarily this instance's (one started by hand has its own).
  Write(SiteProfileDir(site).Append(kLeftFileName), std::move(windows),
        std::move(done));
}

void OpenStartupWindows(content::BrowserContext* browser_context,
                        const GURL& startup_url,
                        bool restore_left,
                        bool resume) {
  FileRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath saved, base::FilePath left,
             bool restore_left) -> std::optional<std::string> {
            // Both files hold a JSON list of windows: join them.
            std::vector<std::string> lists;
            for (const base::FilePath& path : {saved, left}) {
              if (path == left && !restore_left) {
                continue;  // kept for when the user comes Back
              }
              std::string json;
              if (base::ReadFileToString(path, &json)) {
                // The left window is restored once. The session stays: the
                // instance keeps it current from here (SessionChanged).
                if (path == left) {
                  base::DeleteFile(path);
                }
                lists.push_back(std::move(json));
              }
            }
            if (lists.empty()) {
              return std::nullopt;
            }
            std::optional<base::ListValue> all = base::ListValue();
            for (const std::string& json : lists) {
              std::optional<base::ListValue> windows =
                  base::JSONReader::ReadList(json, base::JSON_PARSE_RFC);
              if (windows) {
                for (base::Value& window : *windows) {
                  all->Append(std::move(window));
                }
              }
            }
            return base::WriteJson(*all);
          },
          SavedPath(browser_context), LeftPath(browser_context),
          restore_left),
      base::BindOnce(&OnSavedRead, browser_context, startup_url, resume));
}

}  // namespace lrb
