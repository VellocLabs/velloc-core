// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_types.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(LlmTypesTest, TranslateOpenAiFinishReasonKnownValues) {
  EXPECT_EQ(TranslateOpenAiFinishReason("stop"), FinishReason::kStop);
  EXPECT_EQ(TranslateOpenAiFinishReason("length"), FinishReason::kLength);
  EXPECT_EQ(TranslateOpenAiFinishReason("content_filter"),
            FinishReason::kContentFilter);
  EXPECT_EQ(TranslateOpenAiFinishReason("tool_calls"),
            FinishReason::kToolCalls);
}

TEST(LlmTypesTest, TranslateOpenAiFinishReasonUnknownFallsThroughToOther) {
  EXPECT_EQ(TranslateOpenAiFinishReason(""), FinishReason::kOther);
  EXPECT_EQ(TranslateOpenAiFinishReason("function_call"), FinishReason::kOther);
  EXPECT_EQ(TranslateOpenAiFinishReason("STOP"), FinishReason::kOther);
  EXPECT_EQ(TranslateOpenAiFinishReason("end_turn"), FinishReason::kOther);
}

}  // namespace
}  // namespace nexus
