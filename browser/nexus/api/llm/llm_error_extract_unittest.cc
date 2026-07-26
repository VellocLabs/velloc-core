// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/llm_error_extract.h"

#include "base/strings/string_number_conversions.h"
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

// BuildProviderErrorMessage is the only channel the guest has for classifying
// a provider failure: the host->guest LLM contract carries errors as a plain
// string, so a status code or Retry-After that isn't in the text is lost. The
// guest's classifier (nexus_web/src/utils/providerConnectivity.ts) reads
// "HTTP <status>" and "retry-after: <n>" out of exactly this format.
TEST(LlmErrorExtractTest, BuildsStatusPrefixedErrorMessage) {
  EXPECT_EQ(BuildProviderErrorMessage(429, "", "Too many requests"),
            "HTTP 429: Too many requests");
  // No body message: status alone (the pre-existing "HTTP <code>" shape).
  EXPECT_EQ(BuildProviderErrorMessage(500, "", ""), "HTTP 500");
  // No status (net error / no headers): the message passes through unchanged,
  // so a provider that cannot supply a status is no worse off than before.
  EXPECT_EQ(BuildProviderErrorMessage(-1, "", "Connection reset"),
            "Connection reset");
  EXPECT_EQ(BuildProviderErrorMessage(0, "", "Connection reset"),
            "Connection reset");
}

TEST(LlmErrorExtractTest, ForwardsDeltaSecondsRetryAfter) {
  EXPECT_EQ(BuildProviderErrorMessage(429, "30", "Rate limit exceeded"),
            "HTTP 429: Rate limit exceeded (retry-after: 30s)");
  EXPECT_EQ(BuildProviderErrorMessage(503, "5", ""),
            "HTTP 503 (retry-after: 5s)");
  // HTTP-date and garbage forms are dropped rather than forwarded as text the
  // guest would mis-parse.
  EXPECT_EQ(BuildProviderErrorMessage(429, "Wed, 21 Oct 2026 07:28:00 GMT",
                                      "Rate limit exceeded"),
            "HTTP 429: Rate limit exceeded");
  EXPECT_EQ(BuildProviderErrorMessage(429, "-5", "Rate limit exceeded"),
            "HTTP 429: Rate limit exceeded");
  EXPECT_EQ(BuildProviderErrorMessage(429, "soon", "Rate limit exceeded"),
            "HTTP 429: Rate limit exceeded");
}

// ParseProviderErrorMessage is the formatter's inverse: it is how the host
// recovers typed fields (NexusLlmStreamEvent.error_http_status /
// error_retry_after_seconds) from the provider->host string hop before the
// wire. The round-trip MUST be exact — a drift between the two functions
// silently downgrades the guest back to string classification.
TEST(LlmErrorExtractTest, ParseRoundTripsEveryBuildShape) {
  struct Case {
    int status;
    const char* retry_after;
    const char* message;
  };
  const Case cases[] = {
      {429, "30", "Rate limit exceeded"},
      {500, "", ""},
      {503, "5", ""},
      {-1, "", "Connection reset"},
      {-1, "7", "Connection reset"},
      {200, "0", "odd but legal"},
  };
  for (const Case& c : cases) {
    const std::string built =
        BuildProviderErrorMessage(c.status, c.retry_after, c.message);
    const ParsedProviderError parsed = ParseProviderErrorMessage(built);
    const bool has_status = c.status >= 100 && c.status <= 599;
    EXPECT_EQ(parsed.http_status.has_value(), has_status) << built;
    if (has_status) {
      EXPECT_EQ(*parsed.http_status, c.status) << built;
    }
    const bool has_retry = c.retry_after[0] != '\0';
    EXPECT_EQ(parsed.retry_after_seconds.has_value(), has_retry) << built;
    if (has_retry) {
      int expected = 0;
      ASSERT_TRUE(base::StringToInt(c.retry_after, &expected));
      EXPECT_EQ(*parsed.retry_after_seconds, expected) << built;
    }
  }
}

TEST(LlmErrorExtractTest, ParseRejectsNonCanonicalShapes) {
  // A 3-digit number anywhere but the anchored prefix is NOT a status.
  EXPECT_FALSE(ParseProviderErrorMessage("no chunks for 190s").http_status);
  EXPECT_FALSE(
      ParseProviderErrorMessage("model needs 512 MiB more").http_status);
  EXPECT_FALSE(ParseProviderErrorMessage("HTTP 99: too short").http_status);
  EXPECT_FALSE(ParseProviderErrorMessage("HTTP 6000 nope").http_status);
  // "HTTP 429x" — 4th char neither ':' / ' ' / end.
  EXPECT_FALSE(ParseProviderErrorMessage("HTTP 429x").http_status);
  // retry-after must be the terminal suffix with numeric seconds.
  EXPECT_FALSE(ParseProviderErrorMessage(
                   "(retry-after: 5s) but then more text")
                   .retry_after_seconds);
  EXPECT_FALSE(
      ParseProviderErrorMessage("(retry-after: soons)").retry_after_seconds);
  // Mid-string decoy + real terminal suffix: the terminal one wins.
  const ParsedProviderError decoy = ParseProviderErrorMessage(
      "HTTP 429: body quoted \"(retry-after: 99s)\" here (retry-after: 3s)");
  ASSERT_TRUE(decoy.retry_after_seconds.has_value());
  EXPECT_EQ(*decoy.retry_after_seconds, 3);
  ASSERT_TRUE(decoy.http_status.has_value());
  EXPECT_EQ(*decoy.http_status, 429);
}

}  // namespace nexus
