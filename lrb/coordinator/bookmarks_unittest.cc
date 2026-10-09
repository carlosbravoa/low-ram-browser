// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/bookmarks.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace lrb::coordinator {
namespace {

std::vector<std::string> Urls(const std::vector<Suggestion>& suggestions) {
  std::vector<std::string> urls;
  for (const Suggestion& suggestion : suggestions) {
    urls.push_back(suggestion.url);
  }
  return urls;
}

TEST(CoordinatorBookmarks, FileRoundTrip) {
  const std::vector<Bookmark> bookmarks = {
      {"https://a.example/", "A page"}, {"https://b.example/x?y=1", ""}};
  EXPECT_EQ(SerializeBookmarks(bookmarks),
            "https://a.example/\tA page\nhttps://b.example/x?y=1\t\n");
  const std::vector<Bookmark> parsed =
      ParseBookmarks(SerializeBookmarks(bookmarks));
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[1].url, "https://b.example/x?y=1");
  EXPECT_EQ(parsed[0].title, "A page");
  // Blank and malformed lines are skipped, and URLs too long to send.
  EXPECT_EQ(ParseBookmarks("\n\tno url\nhttps://c.example/\n").size(), 1u);
  EXPECT_TRUE(ParseBookmarks("https://c.example/" + std::string(7000, 'x') +
                             "\tlong\n")
                  .empty());
}

TEST(CoordinatorBookmarks, CleanTitle) {
  EXPECT_EQ(CleanTitle("  a\tb\nc\r  "), "a b c");
  EXPECT_EQ(CleanTitle(std::string(400, 'x')).size(), 300u);
  // Not cut inside a character: 299 'x' then a 2-byte "é".
  EXPECT_EQ(CleanTitle(std::string(299, 'x') + "\xc3\xa9").size(), 299u);
}

TEST(CoordinatorBookmarks, Encoding) {
  EXPECT_EQ(EncodeText("a b\n%/é"), "a%20b%0A%25/%C3%A9");
  EXPECT_EQ(DecodeText(EncodeText("a b\n%/é")), "a b\n%/é");
  EXPECT_EQ(DecodeText("100%"), "100%");  // not an escape
  EXPECT_EQ(DecodeText("%zz%4"), "%zz%4");
}

TEST(CoordinatorBookmarks, Suggest) {
  const std::vector<Bookmark> bookmarks = {
      {"https://www.bbc.co.uk/news", "BBC News - Home"},
      {"https://example.org/recipes", "Grandma's bread"},
      {"https://docs.example.org/", "Team docs"},
  };
  const std::vector<std::string> sites = {"github.com", "bbc.co.uk",
                                          "gitlab.com", "example.org"};
  // Address prefix: the bookmark (www. ignored), then the site.
  EXPECT_EQ(Urls(Suggest("bbc", bookmarks, sites, 8)),
            (std::vector<std::string>{"https://www.bbc.co.uk/news",
                                      "https://bbc.co.uk/"}));
  // Sites in their order (most recent first); typed scheme ignored.
  EXPECT_EQ(Urls(Suggest("https://git", bookmarks, sites, 8)),
            (std::vector<std::string>{"https://github.com/",
                                      "https://gitlab.com/"}));
  // A title word, then a match inside an address or title.
  EXPECT_EQ(Urls(Suggest("BREAD", bookmarks, sites, 8)),
            (std::vector<std::string>{"https://example.org/recipes"}));
  EXPECT_EQ(Urls(Suggest("docs", bookmarks, sites, 8)),
            (std::vector<std::string>{"https://docs.example.org/"}));
  EXPECT_EQ(Urls(Suggest("cipes", bookmarks, sites, 8)),
            (std::vector<std::string>{"https://example.org/recipes"}));
  // Inside a word only from 3 characters ("g" is in ".org").
  EXPECT_EQ(Urls(Suggest("g", bookmarks, {}, 8)),
            (std::vector<std::string>{"https://example.org/recipes"}));
  // Sites only by their name's start.
  EXPECT_TRUE(Suggest("hub", {}, sites, 8).empty());
  EXPECT_EQ(Suggest("g", bookmarks, sites, 1).size(), 1u);
  EXPECT_TRUE(Suggest("  ", bookmarks, sites, 8).empty());
  EXPECT_TRUE(Suggest("www.", bookmarks, sites, 8).empty());
}

TEST(CoordinatorBookmarks, DisplayAddress) {
  EXPECT_EQ(DisplayAddress("https://www.example.org/a"), "example.org/a");
  EXPECT_EQ(DisplayAddress("http://example.org/"), "example.org/");
  EXPECT_EQ(DisplayAddress("example.org"), "example.org");
}

}  // namespace
}  // namespace lrb::coordinator
