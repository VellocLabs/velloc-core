// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_stop_reason.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(AnthropicStopReasonTest, RefusalMapsToContentFilter) {
  // The Fable 5 / 5.1 / Opus 5 safety-classifier shape: HTTP 200 with
  // `stop_reason: "refusal"`. Before this mapping existed it fell through to
  // kOther and the guest showed an empty "unknown" stop.
  EXPECT_EQ(TranslateAnthropicStopReason("refusal"),
            FinishReason::kContentFilter);
}

TEST(AnthropicStopReasonTest, KnownValues) {
  EXPECT_EQ(TranslateAnthropicStopReason("end_turn"), FinishReason::kStop);
  EXPECT_EQ(TranslateAnthropicStopReason("stop_sequence"),
            FinishReason::kStop);
  EXPECT_EQ(TranslateAnthropicStopReason("max_tokens"),
            FinishReason::kLength);
  EXPECT_EQ(TranslateAnthropicStopReason("tool_use"),
            FinishReason::kToolCalls);
}

TEST(AnthropicStopReasonTest, UnknownFallsThroughToOther) {
  EXPECT_EQ(TranslateAnthropicStopReason(""), FinishReason::kOther);
  EXPECT_EQ(TranslateAnthropicStopReason("pause_turn"), FinishReason::kOther);
  EXPECT_EQ(TranslateAnthropicStopReason("REFUSAL"), FinishReason::kOther);
  EXPECT_EQ(TranslateAnthropicStopReason("content_filter"),
            FinishReason::kOther);
}

TEST(AnthropicStopReasonTest, StopDetailsCopiesStringFields) {
  base::DictValue raw;
  raw.Set("type", "refusal");
  raw.Set("category", "cyber");
  raw.Set("explanation", "This request was declined by a safety classifier.");
  raw.Set("recommended_model", "claude-opus-4-8");
  base::Value value(std::move(raw));

  std::optional<base::DictValue> details =
      NormalizeAnthropicStopDetails(&value);
  ASSERT_TRUE(details.has_value());
  EXPECT_EQ(details->size(), 4u);
  ASSERT_NE(details->FindString("type"), nullptr);
  EXPECT_EQ(*details->FindString("type"), "refusal");
  ASSERT_NE(details->FindString("category"), nullptr);
  EXPECT_EQ(*details->FindString("category"), "cyber");
  ASSERT_NE(details->FindString("explanation"), nullptr);
  EXPECT_EQ(*details->FindString("explanation"),
            "This request was declined by a safety classifier.");
  ASSERT_NE(details->FindString("recommended_model"), nullptr);
  EXPECT_EQ(*details->FindString("recommended_model"), "claude-opus-4-8");
}

TEST(AnthropicStopReasonTest, StopDetailsSkipsNullAndNonStringFields) {
  // Anthropic documents `category` / `explanation` as nullable, and the
  // category set is open — the normalizer must neither crash on nor invent a
  // value for a null.
  base::DictValue raw;
  raw.Set("type", "refusal");
  raw.Set("category", base::Value());
  raw.Set("explanation", 42);
  raw.Set("unrelated", "dropped");
  base::Value value(std::move(raw));

  std::optional<base::DictValue> details =
      NormalizeAnthropicStopDetails(&value);
  ASSERT_TRUE(details.has_value());
  EXPECT_EQ(details->size(), 1u);
  EXPECT_EQ(details->FindString("category"), nullptr);
  EXPECT_EQ(details->FindString("explanation"), nullptr);
  EXPECT_EQ(details->FindString("unrelated"), nullptr);
  ASSERT_NE(details->FindString("type"), nullptr);
  EXPECT_EQ(*details->FindString("type"), "refusal");
}

TEST(AnthropicStopReasonTest, StopDetailsAbsentNullOrEmptyYieldsNothing) {
  EXPECT_FALSE(NormalizeAnthropicStopDetails(nullptr).has_value());

  base::Value null_value;
  EXPECT_FALSE(NormalizeAnthropicStopDetails(&null_value).has_value());

  base::Value string_value("refusal");
  EXPECT_FALSE(NormalizeAnthropicStopDetails(&string_value).has_value());

  base::Value empty_dict{base::DictValue()};
  EXPECT_FALSE(NormalizeAnthropicStopDetails(&empty_dict).has_value());

  base::DictValue only_empty_strings;
  only_empty_strings.Set("category", "");
  base::Value empty_strings_value(std::move(only_empty_strings));
  EXPECT_FALSE(NormalizeAnthropicStopDetails(&empty_strings_value).has_value());
}

}  // namespace
}  // namespace nexus
