// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/page_script/page_script_options.h"

#include <string>

#include "base/strings/strcat.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(PageScriptOptionsTest, WorldNamesParse) {
  EXPECT_EQ(PageScriptWorld::kIsolated, ParsePageScriptWorld(std::nullopt));
  EXPECT_EQ(PageScriptWorld::kIsolated, ParsePageScriptWorld("isolated"));
  EXPECT_EQ(PageScriptWorld::kMain, ParsePageScriptWorld("main"));
  EXPECT_FALSE(ParsePageScriptWorld("page").has_value());
  EXPECT_FALSE(ParsePageScriptWorld("").has_value());

  EXPECT_EQ("isolated", PageScriptWorldName(PageScriptWorld::kIsolated));
  EXPECT_EQ("main", PageScriptWorldName(PageScriptWorld::kMain));
}

TEST(PageScriptOptionsTest, ExactlyOneTargetIsRequired) {
  EXPECT_TRUE(ValidatePageScriptTarget({}).has_value());

  PageScriptTargetRequest window;
  window.web_window_id = 4;
  EXPECT_FALSE(ValidatePageScriptTarget(window).has_value());

  PageScriptTargetRequest tab;
  tab.tab_id = 9;
  EXPECT_FALSE(ValidatePageScriptTarget(tab).has_value());

  PageScriptTargetRequest both;
  both.web_window_id = 4;
  both.tab_id = 9;
  EXPECT_TRUE(ValidatePageScriptTarget(both).has_value());
}

TEST(PageScriptOptionsTest, PayloadLimitIsOneMegabyte) {
  EXPECT_TRUE(IsPageScriptPayloadWithinLimit(""));
  EXPECT_TRUE(IsPageScriptPayloadWithinLimit(
      std::string(kPageScriptMaxPayloadBytes, 'x')));
  EXPECT_FALSE(IsPageScriptPayloadWithinLimit(
      std::string(kPageScriptMaxPayloadBytes + 1, 'x')));
}

TEST(PageScriptOptionsTest, PrivateBootstrapSendsReceivesAndDispatches) {
  const std::string source =
      PageScriptBootstrapSource(PageScriptWorld::kIsolated);
  EXPECT_NE(std::string::npos, source.find("vellocHost"));
  EXPECT_NE(std::string::npos, source.find(kPageScriptBindingName));
  EXPECT_EQ(std::string::npos, source.find(kPageScriptPageBindingName));
  EXPECT_NE(std::string::npos, source.find("onMessage"));
  EXPECT_NE(std::string::npos, source.find(kPageScriptDispatchName));
}

TEST(PageScriptOptionsTest, PageBootstrapSendsAndReceivesOverThePageCall) {
  const std::string source = PageScriptBootstrapSource(PageScriptWorld::kMain);
  EXPECT_NE(std::string::npos, source.find("vellocHost"));
  EXPECT_NE(std::string::npos, source.find(kPageScriptPageBindingName));
  // No way to the private host call.
  EXPECT_EQ(std::string::npos,
            source.find(base::StrCat({kPageScriptBindingName, ";"})));
  EXPECT_NE(std::string::npos, source.find("onMessage"));
  EXPECT_NE(std::string::npos, source.find(kPageScriptDispatchName));
}

TEST(PageScriptOptionsTest, DispatchExpressionEmbedsTheMessageAsAString) {
  const std::string expression =
      PageScriptDispatchExpression(R"({"a":"x\"y</script>"})");
  EXPECT_NE(std::string::npos, expression.find(kPageScriptDispatchName));
  // The message rides inside one string literal: its quotes are escaped and
  // the closing tag cannot end a script.
  EXPECT_NE(std::string::npos, expression.find("\\\"a\\\""));
  EXPECT_EQ(std::string::npos, expression.find("</script>"));
}

}  // namespace
}  // namespace nexus
