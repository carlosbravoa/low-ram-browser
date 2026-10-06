// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_SETTINGS_H_
#define LRB_COMMON_SETTINGS_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "url/gurl.h"

namespace lrb {

// The user's settings, one file shared by every site's windows
// ($XDG_CONFIG_HOME/lrb/settings.json, else ~/.config/lrb/settings.json).
// Small and read rarely: instances read it at start (rendering) and when
// it matters (each search), so a change applies to every window without
// any of them watching the file.
struct Settings {
  // Where searches go: a URL with %s for the search terms.
  std::string search_url;
  // GPU (true) or software rendering (false); unset: never chosen
  // (software, the leaner, until the user chooses).
  std::optional<bool> gpu;

  // Blocking: reads and writes the file. Read() parses no URLs: it runs
  // at startup, before URL schemes are registered.
  static Settings Read();
  static bool Write(const Settings& settings);
  static base::FilePath File();
};

// Search engines offered in the settings. The first is the default:
// DuckDuckGo's HTML version, no script, the cheapest results page.
struct SearchEngine {
  const char16_t* name;
  const char* url;  // with %s
};
base::span<const SearchEngine> SearchEngines();
inline constexpr char kDefaultSearchUrl[] =
    "https://html.duckduckgo.com/html/?q=%s";

// A usable search URL: http(s), with %s.
bool IsValidSearchUrl(std::string_view url);

// `search_url` with `terms` in place of %s (escaped); the default if
// `search_url` isn't usable.
GURL SearchFor(std::string_view search_url, std::u16string_view terms);

}  // namespace lrb

#endif  // LRB_COMMON_SETTINGS_H_
