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

TEST(CoordinatorRules, ExternalAppUrls) {
  EXPECT_EQ(ExternalAppScheme("mailto:someone@example.com"), "mailto");
  EXPECT_EQ(ExternalAppScheme("MAILTO:someone@example.com"), "mailto");
  EXPECT_EQ(ExternalAppScheme("tel:+56912345678"), "tel");
  EXPECT_EQ(ExternalAppScheme("zoommtg://zoom.us/join?confno=1"), "zoommtg");
  EXPECT_EQ(ExternalAppScheme("ms-teams:/l/meetup-join/x"), "ms-teams");
  EXPECT_EQ(ExternalAppScheme("web+app:x"), "web+app");
  EXPECT_TRUE(IsExternalAppUrl("magnet:?xt=urn:btih:abc"));
}

TEST(CoordinatorRules, NotExternalAppUrls) {
  EXPECT_FALSE(IsExternalAppUrl("https://example.com/"));
  EXPECT_FALSE(IsExternalAppUrl("HTTP://example.com/"));
  EXPECT_FALSE(IsExternalAppUrl("file:///etc/passwd"));
  EXPECT_FALSE(IsExternalAppUrl("javascript:alert(1)"));
  EXPECT_FALSE(IsExternalAppUrl("data:text/html,x"));
  EXPECT_FALSE(IsExternalAppUrl("chrome://settings"));
  EXPECT_FALSE(IsExternalAppUrl("mailto:"));               // nothing after
  EXPECT_FALSE(IsExternalAppUrl(":x"));                    // no scheme
  EXPECT_FALSE(IsExternalAppUrl("1tel:123"));              // starts with digit
  EXPECT_FALSE(IsExternalAppUrl("-x:y"));                  // an option for xdg-open
  EXPECT_FALSE(IsExternalAppUrl("mailto:a b"));            // whitespace
  EXPECT_FALSE(IsExternalAppUrl("mailto:a\nopen-app x"));  // control
  EXPECT_FALSE(IsExternalAppUrl("noscheme"));
  EXPECT_FALSE(IsExternalAppUrl("mailto:" + std::string(4096, 'a')));
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

TEST(CoordinatorRules, JsonString) {
  const std::string json =
      R"({"gpu":false,"search_url":"https://a.example/?q=%s",)"
      R"("startup": "page", "startup_page":"https:\/\/b.example\/x\u003Cy\"z"})";
  EXPECT_EQ("page", JsonString(json, "startup"));
  EXPECT_EQ("https://b.example/x<y\"z", JsonString(json, "startup_page"));
  EXPECT_EQ("https://a.example/?q=%s", JsonString(json, "search_url"));
  EXPECT_FALSE(JsonString(json, "gpu"));               // not a string
  EXPECT_FALSE(JsonString(json, "content_blocking"));  // missing
  EXPECT_FALSE(JsonString(R"({"a":"b)", "a"));          // unterminated
  EXPECT_FALSE(JsonString(R"({"a":"\u00e9"})", "a"));  // not ASCII
  EXPECT_FALSE(JsonString("{\"a\":\"x\ny\"}", "a"));    // raw control
}

TEST(CoordinatorRules, JsonBool) {
  const std::string json =
      R"({"gpu":false, "ask_downloads" : true,"show_memory":false})"
      R"(,"startup":"page","x":truex,"n":1})";
  EXPECT_EQ(false, JsonBool(json, "gpu"));
  EXPECT_EQ(true, JsonBool(json, "ask_downloads"));
  EXPECT_EQ(false, JsonBool(json, "show_memory"));
  EXPECT_FALSE(JsonBool(json, "startup").has_value());  // a string
  EXPECT_FALSE(JsonBool(json, "x").has_value());        // not a literal
  EXPECT_FALSE(JsonBool(json, "n").has_value());        // a number
  EXPECT_FALSE(JsonBool(json, "missing").has_value());
  EXPECT_EQ(true, JsonBool(R"({"a":true})", "a"));      // last in the object
}

}  // namespace lrb::coordinator
