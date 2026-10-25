// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/ui/utility_tab_options.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {

TEST(UtilityTabOptionsTest, KeysAreShortLowercaseSlugs) {
  EXPECT_TRUE(IsUtilityTabKey("customize"));
  EXPECT_TRUE(IsUtilityTabKey("a-1"));
  EXPECT_TRUE(IsUtilityTabKey(std::string(32, 'k')));
  EXPECT_FALSE(IsUtilityTabKey(""));
  EXPECT_FALSE(IsUtilityTabKey(std::string(33, 'k')));
  EXPECT_FALSE(IsUtilityTabKey("Customize"));
  EXPECT_FALSE(IsUtilityTabKey("with space"));
  EXPECT_FALSE(IsUtilityTabKey("under_score"));
  EXPECT_FALSE(IsUtilityTabKey("dot.key"));
}

TEST(UtilityTabOptionsTest, OnlyWebUrlsLoad) {
  EXPECT_TRUE(IsUtilityTabUrl(GURL("https://example.com/customize/skills")));
  EXPECT_TRUE(IsUtilityTabUrl(GURL("http://127.0.0.1:8080/")));
  EXPECT_FALSE(IsUtilityTabUrl(GURL("chrome://settings")));
  EXPECT_FALSE(IsUtilityTabUrl(GURL("file:///C:/x.html")));
  EXPECT_FALSE(IsUtilityTabUrl(GURL("javascript:alert(1)")));
  EXPECT_FALSE(IsUtilityTabUrl(GURL("about:blank")));
  EXPECT_FALSE(IsUtilityTabUrl(GURL("not a url")));
  EXPECT_FALSE(IsUtilityTabUrl(GURL()));
}

}  // namespace nexus
