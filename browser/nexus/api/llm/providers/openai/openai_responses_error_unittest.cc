// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_responses_error.h"

#include <string>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue ParseDict(const std::string& json) {
  auto parsed = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  EXPECT_TRUE(parsed.has_value()) << "bad test JSON: " << json;
  return parsed.has_value() ? std::move(*parsed) : base::DictValue();
}

// The regression this file exists for. Captured live 2026-08-11 from
// chatgpt-pro / gpt-5.6-luna: the ChatGPT (Codex) backend reports a capacity
// outage as a mid-stream `error` event carrying OpenAI's NON-streaming
// envelope, so the message sits at `error.message`, NOT flat at `message`.
//
// The old parser read only the flat key, found nothing, and emitted the
// placeholder "Responses API stream error" — which the guest cannot classify,
// so the turn died with "Issue: unknown" and errorRetryCount 0 instead of
// being retried. Observed at a ~35% stream failure rate during the outage
// window; every one of those turns was recoverable.
TEST(OpenAiResponsesErrorTest, ReadsCodexNestedErrorEnvelope) {
  const base::DictValue event = ParseDict(R"({
    "error": {
      "code": "server_is_overloaded",
      "message": "Our servers are currently overloaded. Please try again later.",
      "param": null,
      "type": "service_unavailable_error"
    },
    "sequence_number": 2,
    "type": "error"
  })");

  EXPECT_EQ(ExtractResponsesApiErrorMessage(event),
            "Our servers are currently overloaded. Please try again later.");
  EXPECT_EQ(ExtractResponsesApiErrorCode(event), "server_is_overloaded");

  bool recognized = false;
  EXPECT_EQ(BuildResponsesApiErrorMessage(event, &recognized),
            "server_is_overloaded: Our servers are currently overloaded. "
            "Please try again later.");
  EXPECT_TRUE(recognized);
}

// The documented `response.failed` shape. The same failure emits this ~15ms
// AFTER the `error` event above; only the first terminal event is queued, so
// both must extract correctly or the race decides what the user sees.
TEST(OpenAiResponsesErrorTest, ReadsResponseFailedShape) {
  const base::DictValue event = ParseDict(R"({
    "response": {
      "id": "resp_abc",
      "error": {
        "code": "server_is_overloaded",
        "message": "Our servers are currently overloaded. Please try again later."
      }
    },
    "type": "response.failed"
  })");

  bool recognized = false;
  EXPECT_EQ(BuildResponsesApiErrorMessage(event, &recognized),
            "server_is_overloaded: Our servers are currently overloaded. "
            "Please try again later.");
  EXPECT_TRUE(recognized);
}

// The documented bare `error` event — message flat, no envelope. This is the
// shape the old code handled, and it must keep working.
TEST(OpenAiResponsesErrorTest, ReadsFlatMessageShape) {
  const base::DictValue event = ParseDict(R"({
    "type": "error",
    "message": "Something went wrong.",
    "code": "server_error"
  })");

  EXPECT_EQ(ExtractResponsesApiErrorMessage(event), "Something went wrong.");
  EXPECT_EQ(ExtractResponsesApiErrorCode(event), "server_error");
  bool recognized = false;
  EXPECT_EQ(BuildResponsesApiErrorMessage(event, &recognized),
            "server_error: Something went wrong.");
  EXPECT_TRUE(recognized);
}

// `error` as a bare string rather than an object.
TEST(OpenAiResponsesErrorTest, ReadsErrorAsBareString) {
  const base::DictValue event = ParseDict(R"({
    "type": "error",
    "error": "upstream connect failure"
  })");

  bool recognized = false;
  EXPECT_EQ(BuildResponsesApiErrorMessage(event, &recognized),
            "upstream connect failure");
  EXPECT_TRUE(recognized);
}

// The top-level `type` of a bare error event is the literal "error" — it is
// not a code and must never be prefixed onto the message.
TEST(OpenAiResponsesErrorTest, DoesNotTreatTopLevelTypeAsCode) {
  const base::DictValue event = ParseDict(R"({
    "type": "error",
    "message": "plain failure"
  })");

  EXPECT_EQ(ExtractResponsesApiErrorCode(event), "");
  bool recognized = false;
  EXPECT_EQ(BuildResponsesApiErrorMessage(event, &recognized), "plain failure");
}

// A code already spelled in the message is not repeated as a prefix.
TEST(OpenAiResponsesErrorTest, DoesNotDoublePrefixCode) {
  const base::DictValue event = ParseDict(R"({
    "error": {
      "code": "rate_limit_exceeded",
      "message": "rate_limit_exceeded: slow down"
    }
  })");

  bool recognized = false;
  EXPECT_EQ(BuildResponsesApiErrorMessage(event, &recognized),
            "rate_limit_exceeded: slow down");
}

// The whole point of the change: a shape nobody anticipated must PRESERVE the
// event rather than collapse to a placeholder, so the next occurrence is
// diagnosable from one log line and the text still reaches the guest.
TEST(OpenAiResponsesErrorTest, PreservesUnrecognizedEventVerbatim) {
  const base::DictValue event = ParseDict(R"({
    "type": "error",
    "detail": {"nested": "in a shape we do not know"}
  })");

  bool recognized = true;
  const std::string built = BuildResponsesApiErrorMessage(event, &recognized);
  EXPECT_FALSE(recognized);
  EXPECT_NE(built.find("unrecognized error event:"), std::string::npos);
  EXPECT_NE(built.find("in a shape we do not know"), std::string::npos);
}

// A pathological payload is bounded, not passed through whole — it ends up in
// the transcript.
TEST(OpenAiResponsesErrorTest, BoundsAnEnormousUnrecognizedEvent) {
  base::DictValue event;
  event.Set("type", "error");
  event.Set("junk", std::string(8 * 1024, 'x'));

  const std::string built = BuildResponsesApiErrorMessage(event, nullptr);
  EXPECT_LT(built.size(), kMaxSerializedErrorEventChars + 128);
  EXPECT_NE(built.find("...(truncated)"), std::string::npos);
}

// A `response.failed` event echoes the whole Response back, including
// `instructions` — which this provider sets to the SYSTEM PROMPT (project
// paths, memory/CLAUDE.md excerpts) — and `input`/`output`, the conversation.
// The diagnostic string is written to chrome_debug.log on every failed stream
// and, for an unrecognized shape, reaches the visible transcript, so the
// payload must be pruned away while the error itself is kept.
TEST(OpenAiResponsesErrorTest, PrunesRequestPayloadFromTheDiagnosticEvent) {
  const base::DictValue event = ParseDict(R"({
    "type": "response.failed",
    "response": {
      "id": "resp_abc",
      "status": "failed",
      "instructions": "SYSTEM PROMPT WITH D:/private/path AND MEMORY",
      "input": [{"role": "user", "content": "the user's secret question"}],
      "output": [{"text": "a partial answer"}],
      "error": {"code": "server_is_overloaded", "message": "overloaded"}
    }
  })");

  const std::string diag = SerializeEventForDiagnosis(event);

  EXPECT_EQ(diag.find("SYSTEM PROMPT"), std::string::npos) << diag;
  EXPECT_EQ(diag.find("secret question"), std::string::npos) << diag;
  EXPECT_EQ(diag.find("partial answer"), std::string::npos) << diag;
  // The diagnostic value survives.
  EXPECT_NE(diag.find("server_is_overloaded"), std::string::npos) << diag;
  EXPECT_NE(diag.find("resp_abc"), std::string::npos) << diag;
}

// Truncation must land on a codepoint boundary. The string ends up in
// `NexusLlmStreamEvent.error_message`, a mojo `string?`, and mojo requires
// valid UTF-8 — while base::JSONWriter emits non-ASCII as raw UTF-8 and a
// provider error message can be localized. A plain resize() at the byte cap
// would split a multi-byte sequence here.
TEST(OpenAiResponsesErrorTest, TruncatesUnrecognizedEventOnACodepointBoundary) {
  base::DictValue event;
  event.Set("type", "error");
  // 3 bytes per character, so a byte cap lands mid-sequence for most caps.
  std::string wide;
  for (int i = 0; i < 2000; ++i)
    wide += "\xE4\xB8\xAD";  // U+4E2D
  event.Set("junk", wide);

  const std::string built = BuildResponsesApiErrorMessage(event, nullptr);
  EXPECT_LT(built.size(), kMaxSerializedErrorEventChars + 128);
  EXPECT_NE(built.find("...(truncated)"), std::string::npos);
  EXPECT_TRUE(base::IsStringUTF8(built)) << "truncation split a codepoint";
}

// An empty message field must not count as recognized — otherwise the guest
// gets an empty error string, which classifies as nothing at all.
TEST(OpenAiResponsesErrorTest, TreatsEmptyMessageAsUnrecognized) {
  const base::DictValue event = ParseDict(R"({
    "type": "error",
    "message": "",
    "error": {"message": ""}
  })");

  bool recognized = true;
  const std::string built = BuildResponsesApiErrorMessage(event, &recognized);
  EXPECT_FALSE(recognized);
  EXPECT_FALSE(built.empty());
}

}  // namespace
}  // namespace nexus
