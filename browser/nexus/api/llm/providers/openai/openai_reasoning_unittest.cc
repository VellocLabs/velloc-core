// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_reasoning.h"

#include <optional>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue Parse(const char* json) {
  std::optional<base::DictValue> value =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  CHECK(value) << json;
  return std::move(*value);
}

TEST(OpenAiReasoningTest, ReasoningContentDialect) {
  // DeepSeek / Kimi / Qwen / GLM stream thinking in `reasoning_content`.
  EXPECT_EQ("let me think",
            ExtractChatCompletionReasoningText(Parse(
                R"({"content": "", "reasoning_content": "let me think"})")));
}

TEST(OpenAiReasoningTest, GatewayReasoningDialect) {
  // OpenRouter / Vercel AI Gateway stream it in `reasoning`.
  EXPECT_EQ("step 1", ExtractChatCompletionReasoningText(
                          Parse(R"({"reasoning": "step 1"})")));
}

TEST(OpenAiReasoningTest, GatewaySendsReasoningAndDetailsOnce) {
  // Gateways put the same text in `reasoning` AND `reasoning_details` on one
  // chunk; it must come out once, not twice.
  EXPECT_EQ("Let me think.", ExtractChatCompletionReasoningText(Parse(R"({
    "reasoning": "Let me think.",
    "reasoning_details": [{"type": "reasoning.text",
                           "text": "Let me think.",
                           "signature": "sig",
                           "format": "anthropic-claude-v1"}]})")));
}

TEST(OpenAiReasoningTest, ReasoningContentWinsOverReasoning) {
  // vLLM sends the new `reasoning` beside the deprecated `reasoning_content`
  // with identical text.
  EXPECT_EQ("x", ExtractChatCompletionReasoningText(Parse(
                     R"({"reasoning_content": "x", "reasoning": "x"})")));
}

TEST(OpenAiReasoningTest, EmptyReasoningContentFallsThrough) {
  // Some backends keep `reasoning_content: ""` on every chunk; it must not
  // mask a non-empty `reasoning`.
  EXPECT_EQ("y", ExtractChatCompletionReasoningText(Parse(
                     R"({"reasoning_content": "", "reasoning": "y"})")));
}

TEST(OpenAiReasoningTest, DetailsOnlyTextAndSummary) {
  EXPECT_EQ("ab", ExtractChatCompletionReasoningText(Parse(R"({
    "reasoning_details": [
      {"type": "reasoning.text", "text": "a"},
      {"type": "reasoning.encrypted", "data": "opaque"},
      {"type": "reasoning.summary", "summary": "b"}]})")));
}

TEST(OpenAiReasoningTest, NothingReadableYieldsEmpty) {
  EXPECT_EQ("", ExtractChatCompletionReasoningText(Parse(R"({
    "reasoning_details": [{"type": "reasoning.encrypted", "data": "x"}]})")));
  EXPECT_EQ("", ExtractChatCompletionReasoningText(
                    Parse(R"({"content": "hi", "reasoning": ""})")));
  EXPECT_EQ("", ExtractChatCompletionReasoningText(Parse(
                    R"({"reasoning": null, "reasoning_content": null})")));
}

TEST(OpenAiReasoningTest, ResponsesItemSummaryThenContent) {
  EXPECT_EQ("s1\n\ns2", ExtractResponsesReasoningItemText(Parse(R"({
    "type": "reasoning",
    "summary": [{"type": "summary_text", "text": "s1"},
                {"type": "summary_text", "text": "s2"}],
    "content": [{"type": "reasoning_text", "text": "raw"}]})")));
  EXPECT_EQ("raw", ExtractResponsesReasoningItemText(Parse(R"({
    "type": "reasoning", "summary": [],
    "content": [{"type": "reasoning_text", "text": "raw"}]})")));
  EXPECT_EQ("", ExtractResponsesReasoningItemText(Parse(R"({
    "type": "reasoning", "summary": [], "encrypted_content": "x"})")));
}

TEST(OpenAiReasoningTest, ResponsesReasoningDeltaEvents) {
  EXPECT_TRUE(IsResponsesReasoningTextDeltaEvent(
      "response.reasoning_summary_text.delta"));
  EXPECT_TRUE(
      IsResponsesReasoningTextDeltaEvent("response.reasoning_text.delta"));
  EXPECT_FALSE(
      IsResponsesReasoningTextDeltaEvent("response.output_text.delta"));
  EXPECT_FALSE(IsResponsesReasoningTextDeltaEvent(
      "response.reasoning_summary_text.done"));
}

}  // namespace
}  // namespace nexus
