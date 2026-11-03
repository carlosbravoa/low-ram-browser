// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/dark_pages.h"

#include <set>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/site.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/common/mode.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/mojom/css/preferred_color_scheme.mojom-shared.h"
#include "url/gurl.h"

namespace lrb {

namespace {

// --lrb-dark-pages: every site.
bool g_every_site = false;
// Lean mode: the instance's site.
bool g_dark_pages = false;

// Standard mode: the sites that have them.
std::set<std::string>& DarkSites() {
  static base::NoDestructor<std::set<std::string>> sites;
  return *sites;
}

// Writes in the order they were asked for (a quick on, off).
scoped_refptr<base::SequencedTaskRunner> FileTaskRunner() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *runner;
}

}  // namespace

void LoadDarkPages(const base::FilePath& profile) {
  g_every_site =
      base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kDarkPages);
  if (profile.empty()) {
    return;
  }
  const base::FilePath file = profile.Append(kDarkPagesFile);
  if (!StandardMode()) {
    g_dark_pages = base::PathExists(file);
    return;
  }
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(file, &contents, 1 << 20)) {
    return;
  }
  for (std::string_view line : base::SplitStringPiece(
           contents, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    DarkSites().emplace(line);
  }
}

bool DarkPagesOn(const GURL& page_url) {
  if (g_every_site) {
    return true;
  }
  if (!StandardMode()) {
    return g_dark_pages;
  }
  const std::string site = SiteForUrl(page_url);
  return !site.empty() && DarkSites().contains(site);
}

void SetDarkPages(content::BrowserContext* context,
                  const GURL& page_url,
                  bool on) {
  const base::FilePath file = context->GetPath().Append(kDarkPagesFile);
  if (!StandardMode()) {
    g_dark_pages = on;
    FileTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::FilePath marker, bool on) {
                         if (on) {
                           base::WriteFile(marker, "");
                         } else {
                           base::DeleteFile(marker);
                         }
                       },
                       file, on));
  } else {
    const std::string site = SiteForUrl(page_url);
    if (site.empty()) {
      return;  // not a web page: nothing to keep it for
    }
    if (on) {
      DarkSites().insert(site);
    } else {
      DarkSites().erase(site);
    }
    std::string contents;
    for (const std::string& dark : DarkSites()) {
      contents += dark + "\n";
    }
    FileTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::FilePath file, std::string contents) {
                         if (contents.empty()) {
                           base::DeleteFile(file);
                         } else {
                           base::ImportantFileWriter::WriteFileAtomically(
                               file, contents);
                         }
                       },
                       file, std::move(contents)));
  }
  // Open pages change at once (no reload); tabs not loaded get it when
  // they load. Each page's own site decides (ApplyDarkPages).
  for (Shell* shell : Shell::windows()) {
    shell->web_contents()->OnWebPreferencesChanged();
  }
}

void ApplyDarkPages(blink::web_pref::WebPreferences* prefs,
                    const GURL& page_url) {
  if (!DarkPagesOn(page_url)) {
    return;
  }
  // A page's own dark theme first; Blink darkens the rest.
  prefs->preferred_color_scheme = blink::mojom::PreferredColorScheme::kDark;
  prefs->force_dark_mode_enabled = true;
}

bool RefreshDarkPages(blink::web_pref::WebPreferences* prefs,
                      const GURL& page_url) {
  const bool on = DarkPagesOn(page_url);
  if (on == prefs->force_dark_mode_enabled) {
    return false;
  }
  if (on) {
    ApplyDarkPages(prefs, page_url);
  } else {
    // Back to what the page had without us: blink's default (the page then
    // follows the desktop's theme through the renderer's own preferences),
    // or dark when --force-dark-mode asks for it (OverrideWebPreferences).
    prefs->force_dark_mode_enabled = false;
    prefs->preferred_color_scheme =
        base::CommandLine::ForCurrentProcess()->HasSwitch("force-dark-mode")
            ? blink::mojom::PreferredColorScheme::kDark
            : blink::web_pref::WebPreferences().preferred_color_scheme;
  }
  return true;
}

}  // namespace lrb
