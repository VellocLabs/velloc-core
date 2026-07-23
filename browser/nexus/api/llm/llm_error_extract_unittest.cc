// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/llm_error_extract.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {

TEST(LlmErrorExtractTest, ExtractsOpenAiErrorObjectShape) {
  EXPECT_EQ(
      ExtractApiErrorMessage(
          R"({"error":{"message":"Model not found",)"
          R"("type":"invalid_request_error"}})"),
      "Model not found");
}

TEST(LlmErrorExtractTest, ExtractsAnthropicErrorShape) {
  // Anthropic wraps errors as {"type":"error","error":{"type","message"}} —
  // the same error.message path covers it.
  EXPECT_EQ(
      ExtractApiErrorMessage(
          R"({"type":"error","error":{"type":"overloaded_error",)"
          R"("message":"Overloaded"}})"),
      "Overloaded");
}

TEST(LlmErrorExtractTest, ExtractsOpenAiErrorStringShape) {
  EXPECT_EQ(ExtractApiErrorMessage(R"({"error":"bad api key"})"),
            "bad api key");
}

TEST(LlmErrorExtractTest, ExtractsCodexDetailShape) {
  // ChatGPT subscription (Codex) backends return `{"detail":"..."}` —
  // it must surface verbatim, not as a bare "HTTP <status>".
  EXPECT_EQ(ExtractApiErrorMessage(R"({"detail":"boom"})"), "boom");
  EXPECT_EQ(
      ExtractApiErrorMessage(
          "{\"detail\":\"The 'gpt-x' model is not supported when using "
          "Codex with a ChatGPT account.\"}"),
      "The 'gpt-x' model is not supported when using Codex with a ChatGPT "
      "account.");
}

TEST(LlmErrorExtractTest, ExtractsBareMessageShape) {
  EXPECT_EQ(ExtractApiErrorMessage(R"({"message":"quota exhausted"})"),
            "quota exhausted");
}

// The regression this extraction exists for: the DashScope / token-plan
// gateway returns HTTP 400s as a single SSE `data:` event (live-verified
// 2026-07-23). Before the frame was unwrapped, the streaming path's
// `api_error.empty()` fallback surfaced a bare "HTTP 400" in the guest,
// hiding the actual constraint violation from the user.
TEST(LlmErrorExtractTest, ExtractsErrorFromSseDataFrame) {
  EXPECT_EQ(
      ExtractApiErrorMessage(
          "data: {\"error\":{\"code\":\"invalid_parameter_error\","
          "\"param\":null,\"message\":\"max_completion_tokens [16384] must "
          "be greater than thinking_budget [32768]\","
          "\"type\":\"invalid_request_error\"},\"id\":\"chatcmpl-x\"}\n\n"),
      "max_completion_tokens [16384] must be greater than "
      "thinking_budget [32768]");
}

TEST(LlmErrorExtractTest, HandlesSseFrameWithoutSpaceAndCrlf) {
  EXPECT_EQ(
      ExtractApiErrorMessage(
          "data:{\"error\":{\"message\":\"restricted to True\"}}\r\n\r\n"),
      "restricted to True");
}

TEST(LlmErrorExtractTest, FindsErrorInLaterEventOfMultiEventBody) {
  // A keep-alive/comment line plus a normal chunk, then the error event.
  EXPECT_EQ(
      ExtractApiErrorMessage(
          ": keep-alive\n"
          "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
          "data: {\"error\":{\"message\":\"stream aborted: bad schema\"}}\n\n"),
      "stream aborted: bad schema");
}

TEST(LlmErrorExtractTest, IgnoresNormalStreamContent) {
  EXPECT_EQ(ExtractApiErrorMessage(
                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                "data: [DONE]\n\n"),
            "");
}

TEST(LlmErrorExtractTest, IgnoresSuccessfulAndNonJsonBodies) {
  EXPECT_EQ(ExtractApiErrorMessage(
                R"({"id":"chatcmpl-1","choices":[],"usage":{}})"),
            "");
  EXPECT_EQ(ExtractApiErrorMessage("<html>400 Bad Request</html>"), "");
  EXPECT_EQ(ExtractApiErrorMessage(""), "");
}

}  // namespace nexus
