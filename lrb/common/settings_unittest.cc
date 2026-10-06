// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/common/settings.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace lrb {

TEST(Settings, SearchUrls) {
  EXPECT_TRUE(IsValidSearchUrl("https://example.com/search?q=%s"));
  EXPECT_TRUE(IsValidSearchUrl("http://localhost:8080/?q=%s"));
  EXPECT_FALSE(IsValidSearchUrl("https://example.com/search?q="));  // no %s
  EXPECT_FALSE(IsValidSearchUrl("javascript:alert('%s')"));
  EXPECT_FALSE(IsValidSearchUrl("file:///tmp/%s"));
  EXPECT_FALSE(IsValidSearchUrl("%s"));
  for (const SearchEngine& engine : SearchEngines()) {
    EXPECT_TRUE(IsValidSearchUrl(engine.url)) << engine.url;
  }
  EXPECT_STREQ(kDefaultSearchUrl, SearchEngines()[0].url);
}

TEST(Settings, SearchForEscapesTheTerms) {
  EXPECT_EQ(GURL("https://example.com/?q=a+b%26c%3Dd"),
            SearchFor("https://example.com/?q=%s", u"a b&c=d"));
  // Not a usable address: the default engine.
  EXPECT_EQ(GURL("https://html.duckduckgo.com/html/?q=lrb"),
            SearchFor("javascript:%s", u"lrb"));
}

}  // namespace lrb
