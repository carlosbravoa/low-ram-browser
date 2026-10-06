// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/rules.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace lrb::coordinator {

TEST(CoordinatorRules, ValidSites) {
  EXPECT_TRUE(IsValidSite("example.com"));
  EXPECT_TRUE(IsValidSite("bbc.co.uk"));
  EXPECT_TRUE(IsValidSite("localhost"));
  EXPECT_TRUE(IsValidSite("192.168.0.1"));
  EXPECT_TRUE(IsValidSite("xn--bcher-kva.example"));
}

TEST(CoordinatorRules, SitesThatCouldEscapeTheProfilesDir) {
  EXPECT_FALSE(IsValidSite(""));
  EXPECT_FALSE(IsValidSite("."));
  EXPECT_FALSE(IsValidSite(".."));
  EXPECT_FALSE(IsValidSite("../etc"));
  EXPECT_FALSE(IsValidSite("a/b"));
  EXPECT_FALSE(IsValidSite(".resolver"));
  EXPECT_FALSE(IsValidSite("example..com"));
  EXPECT_FALSE(IsValidSite("example.com."));
  EXPECT_FALSE(IsValidSite("Example.com"));
  EXPECT_FALSE(IsValidSite("exa mple.com"));
  EXPECT_FALSE(IsValidSite(std::string(254, 'a')));
}

TEST(CoordinatorRules, AcceptableUrls) {
  EXPECT_TRUE(IsAcceptableUrl("https://example.com/"));
  EXPECT_TRUE(IsAcceptableUrl("http://example.com:8080/a?b=c#d"));
  EXPECT_FALSE(IsAcceptableUrl("file:///etc/passwd"));
  EXPECT_FALSE(IsAcceptableUrl("javascript:alert(1)"));
  EXPECT_FALSE(IsAcceptableUrl("https://example.com/a b"));
  EXPECT_FALSE(IsAcceptableUrl("https://example.com/\nshow https://x.com"));
  EXPECT_FALSE(IsAcceptableUrl("https:///nohost"));
  EXPECT_FALSE(IsAcceptableUrl("https://" + std::string(9000, 'a')));
}

TEST(CoordinatorRules, HostOf) {
  EXPECT_EQ(HostOf("https://Mail.Google.com/x"), "mail.google.com");
  EXPECT_EQ(HostOf("https://user:pw@example.com:443/"), "example.com");
  EXPECT_EQ(HostOf("https://evil.com@bank.com/"), "bank.com");
  EXPECT_EQ(HostOf("https://bank.com.evil.com/"), "bank.com.evil.com");
  EXPECT_EQ(HostOf("https://example.com./"), "example.com");
  EXPECT_EQ(HostOf("http://[::1]:80/"), "[::1]");
  EXPECT_EQ(HostOf("https://example.com?q"), "example.com");
}

TEST(CoordinatorRules, UrlBelongsToSite) {
  EXPECT_TRUE(UrlBelongsToSite("https://bank.com/login", "bank.com"));
  EXPECT_TRUE(UrlBelongsToSite("https://www.bank.com/", "bank.com"));
  EXPECT_FALSE(UrlBelongsToSite("https://evilbank.com/", "bank.com"));
  EXPECT_FALSE(UrlBelongsToSite("https://bank.com.evil.com/", "bank.com"));
  EXPECT_FALSE(UrlBelongsToSite("https://bank.com@evil.com/", "bank.com"));
  EXPECT_FALSE(UrlBelongsToSite("file:///home/bank.com", "bank.com"));
  EXPECT_FALSE(UrlBelongsToSite("https://bank.com/", "../x"));
}

TEST(CoordinatorRules, WindowBounds) {
  std::optional<WindowBounds> bounds = ParseBounds("-20,40,1000,700");
  ASSERT_TRUE(bounds);
  EXPECT_EQ(-20, bounds->x);
  EXPECT_EQ(40, bounds->y);
  EXPECT_EQ(1000, bounds->width);
  EXPECT_EQ(700, bounds->height);
  EXPECT_EQ("-20,40,1000,700", FormatBounds(*bounds));
  EXPECT_FALSE(ParseBounds(""));
  EXPECT_FALSE(ParseBounds("1,2,300"));
  EXPECT_FALSE(ParseBounds("1,2,300,400,5"));
  EXPECT_FALSE(ParseBounds("1,2,300,400 "));
  EXPECT_FALSE(ParseBounds("1,,300,400"));
  EXPECT_FALSE(ParseBounds("1,2,30,400"));        // too small
  EXPECT_FALSE(ParseBounds("1,2,300,99999"));     // too big
  EXPECT_FALSE(ParseBounds("1,2,300,400\nshow"));
}

}  // namespace lrb::coordinator
