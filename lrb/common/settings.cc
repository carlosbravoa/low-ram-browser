// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/common/settings.h"

#include <memory>

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

namespace lrb {

namespace {

constexpr SearchEngine kSearchEngines[] = {
    {u"DuckDuckGo (no script, lightest)", kDefaultSearchUrl},
    {u"DuckDuckGo", "https://duckduckgo.com/?q=%s"},
    {u"Startpage", "https://www.startpage.com/sp/search?query=%s"},
    {u"Brave Search", "https://search.brave.com/search?q=%s"},
    {u"Ecosia", "https://www.ecosia.org/search?q=%s"},
    {u"Google", "https://www.google.com/search?q=%s"},
    {u"Bing", "https://www.bing.com/search?q=%s"},
};

}  // namespace

// static
base::FilePath Settings::File() {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  base::FilePath config;
  if (std::optional<std::string> xdg = env->GetVar("XDG_CONFIG_HOME");
      xdg && !xdg->empty()) {
    config = base::FilePath(*xdg);
  } else {
    config = base::GetHomeDir().Append(".config");
  }
  return config.Append("lrb").Append("settings.json");
}

// static
Settings Settings::Read() {
  Settings settings;
  settings.search_url = kDefaultSearchUrl;
  std::string json;
  if (!base::ReadFileToString(File(), &json)) {
    return settings;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    LOG(WARNING) << "lrb: unreadable " << File();
    return settings;
  }
  // Checked as text only: Read() runs at startup, before content
  // registers its URL schemes, and parsing a GURL then breaks them (DCHECK
  // "add a scheme after the lists have been used"). SearchFor() parses.
  if (const std::string* url = dict->FindString("search_url");
      url && url->find("%s") != std::string::npos &&
      (url->starts_with("https://") || url->starts_with("http://"))) {
    settings.search_url = *url;
  }
  settings.gpu = dict->FindBool("gpu");
  return settings;
}

// static
bool Settings::Write(const Settings& settings) {
  base::DictValue dict;
  dict.Set("search_url", settings.search_url);
  if (settings.gpu) {
    dict.Set("gpu", *settings.gpu);
  }
  std::optional<std::string> json = base::WriteJson(dict);
  const base::FilePath file = File();
  return json && base::CreateDirectory(file.DirName()) &&
         base::ImportantFileWriter::WriteFileAtomically(file, *json);
}

base::span<const SearchEngine> SearchEngines() {
  return kSearchEngines;
}

bool IsValidSearchUrl(std::string_view url) {
  if (url.find("%s") == std::string_view::npos) {
    return false;
  }
  std::string with_terms(url);
  base::ReplaceFirstSubstringAfterOffset(&with_terms, 0, "%s", "test");
  const GURL test(with_terms);
  return test.is_valid() && test.SchemeIsHTTPOrHTTPS();
}

GURL SearchFor(std::string_view search_url, std::u16string_view terms) {
  std::string url(IsValidSearchUrl(search_url) ? search_url
                                               : kDefaultSearchUrl);
  base::ReplaceFirstSubstringAfterOffset(
      &url, 0, "%s",
      base::EscapeQueryParamValue(base::UTF16ToUTF8(terms),
                                  /*use_plus=*/true));
  return GURL(url);
}

}  // namespace lrb
