// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/dark_pages.h"

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/shell.h"
#include "lrb/common/lrb_switches.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/mojom/css/preferred_color_scheme.mojom-shared.h"

namespace lrb {

namespace {

bool g_dark_pages = false;

}  // namespace

void LoadDarkPages(const base::FilePath& profile) {
  g_dark_pages =
      base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kDarkPages) ||
      (!profile.empty() && base::PathExists(profile.Append(kDarkPagesFile)));
}

bool DarkPagesOn() {
  return g_dark_pages;
}

void SetDarkPages(content::BrowserContext* context, bool on) {
  g_dark_pages = on;
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath marker, bool on) {
            if (on) {
              base::WriteFile(marker, "");
            } else {
              base::DeleteFile(marker);
            }
          },
          context->GetPath().Append(kDarkPagesFile), on));
  // Open pages change at once (no reload); tabs not loaded get it when
  // they load.
  for (Shell* shell : Shell::windows()) {
    shell->web_contents()->OnWebPreferencesChanged();
  }
}

void ApplyDarkPages(blink::web_pref::WebPreferences* prefs) {
  if (!g_dark_pages) {
    return;
  }
  // A page's own dark theme first; Blink darkens the rest.
  prefs->preferred_color_scheme = blink::mojom::PreferredColorScheme::kDark;
  prefs->force_dark_mode_enabled = true;
}

}  // namespace lrb
