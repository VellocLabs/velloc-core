// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/llm/llm_text_helpers.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(LlmTextHelpersTest, AppendFlattenedTextHandlesStrings) {
  std::vector<std::string> out;
  AppendFlattenedText(base::Value("hello"), &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], "hello");
}

TEST(LlmTextHelpersTest, AppendFlattenedTextEncodesEmptyStringAsJsonLiteral) {
  // Empty-string Values fall through the `text && !text->empty()` guard
  // and reach the JSON-serialize branch, which emits the literal `""`.
  // Kept as a regression test — any change to AppendFlattenedText that
  // alters this shape will also change what the LLM sees in system
  // instructions derived from flattened prompt parts.
  std::vector<std::string> out;
  AppendFlattenedText(base::Value(""), &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], "\"\"");
}

TEST(LlmTextHelpersTest, AppendFlattenedTextRecursesIntoLists) {
  base::ListValue list;
  list.Append("alpha");
  list.Append("beta");

  base::ListValue nested;
  nested.Append("gamma");
  list.Append(std::move(nested));

  std::vector<std::string> out;
  AppendFlattenedText(base::Value(std::move(list)), &out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "alpha");
  EXPECT_EQ(out[1], "beta");
  EXPECT_EQ(out[2], "gamma");
}

TEST(LlmTextHelpersTest, AppendFlattenedTextJsonSerializesNonStringValues) {
  base::DictValue dict;
  dict.Set("key", "value");

  std::vector<std::string> out;
  AppendFlattenedText(base::Value(std::move(dict)), &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], "{\"key\":\"value\"}");
}

TEST(LlmTextHelpersTest, AppendFlattenedTextSkipsNoneValue) {
  std::vector<std::string> out;
  AppendFlattenedText(base::Value(), &out);
  EXPECT_TRUE(out.empty());
}

TEST(LlmTextHelpersTest, FlattenTextValueJoinsWithNewlines) {
  base::ListValue list;
  list.Append("first");
  list.Append("second");
  base::Value value(std::move(list));
  EXPECT_EQ(FlattenTextValue(&value), "first\nsecond");
}

TEST(LlmTextHelpersTest, FlattenTextValueReturnsEmptyForNull) {
  EXPECT_TRUE(FlattenTextValue(nullptr).empty());
}

TEST(LlmTextHelpersTest, FlattenTextValueRendersEmptyStringsAsJsonLiteral) {
  // Empty strings inside a list are emitted as the literal "" (see
  // AppendFlattenedTextEncodesEmptyStringAsJsonLiteral above) and show up
  // in the joined result. Regression test for that quirk.
  base::ListValue list;
  list.Append("");
  list.Append("content");
  list.Append("");
  base::Value value(std::move(list));
  EXPECT_EQ(FlattenTextValue(&value), "\"\"\ncontent\n\"\"");
}

TEST(LlmTextHelpersTest, FormatContextSectionReturnsEmptyForNull) {
  EXPECT_TRUE(FormatContextSection("User context", nullptr).empty());
}

TEST(LlmTextHelpersTest, FormatContextSectionReturnsEmptyForEmptyDict) {
  base::DictValue ctx;
  EXPECT_TRUE(FormatContextSection("User context", &ctx).empty());
}

TEST(LlmTextHelpersTest, FormatContextSectionFormatsDictEntries) {
  base::DictValue ctx;
  ctx.Set("url", "https://example.com");
  ctx.Set("title", "Example");
  const std::string formatted =
      FormatContextSection("User context", &ctx);
  // base::DictValue iterates keys in sorted (case-sensitive) order, so
  // "title" comes before "url".
  EXPECT_EQ(formatted,
            "User context:\n- title: Example\n- url: https://example.com");
}

TEST(LlmTextHelpersTest, SerializeValueToJsonProducesRoundTrippableJson) {
  base::DictValue dict;
  dict.Set("k", 42);
  base::Value value(std::move(dict));
  const std::string json = SerializeValueToJson(value);
  auto parsed = ParseJsonValue(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, value);
}

TEST(LlmTextHelpersTest, ParseJsonValueReturnsNullForInvalidJson) {
  EXPECT_FALSE(ParseJsonValue("{not json").has_value());
  EXPECT_FALSE(ParseJsonValue("").has_value());
}

}  // namespace
}  // namespace nexus
