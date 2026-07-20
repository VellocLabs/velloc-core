// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_support.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"

#include <string>

#include "base/test/gtest_util.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {

TEST(NexusLlmHostTest, StreamErrorChunkAcceptsStringErrorPayload) {
  NexusLlmHost host(nullptr);
  NexusLlmHost::StreamState state;
  state.stream_id = "stream-1";
  host.streams_.emplace("stream-1", std::move(state));

  base::DictValue chunk;
  chunk.Set("type", "error");
  chunk.Set("error", "upstream overloaded");

  host.OnStreamChunk("stream-1", std::move(chunk));

  bool callback_called = false;
  bool success = false;
  mojom::NexusLlmStreamReadResultPtr result;
  std::optional<std::string> error_message;
  host.StreamRead(
      "stream-1", /*deadline_ms=*/std::nullopt, /*wait_ms=*/std::nullopt,
      base::DictValue(),
      base::BindOnce(
          [](bool* callback_called, bool* success,
             mojom::NexusLlmStreamReadResultPtr* result,
             std::optional<std::string>* error_message, bool callback_success,
             mojom::NexusLlmStreamReadResultPtr callback_result,
             std::optional<std::string> callback_error_message) {
            *callback_called = true;
            *success = callback_success;
            *result = std::move(callback_result);
            *error_message = std::move(callback_error_message);
          },
          &callback_called, &success, &result, &error_message));

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(success);
  EXPECT_FALSE(error_message.has_value());
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->done);
  ASSERT_EQ(result->events.size(), 1u);
  ASSERT_TRUE(result->events[0]);
  EXPECT_EQ(result->events[0]->type, mojom::NexusLlmStreamEventType::kError);
  EXPECT_EQ(result->events[0]->error_message,
            std::optional<std::string>("upstream overloaded"));
}

// Hold-read leak regressions. These drive the real StreamRead hold path (which
// arms a one-shot hold timer, hence MOCK_TIME) and assert the stream entry is
// erased once the guest can no longer reach it. Before the fix, OnStreamDone
// (stream finishes while a read is held) and OnStreamReadHoldTimeout (hold
// window expires with no data) resolved the held read but left the StreamState
// in `streams_` forever - the guest stops reading on `done`/`timeout` and sends
// no cancel - leaking one entry per stream over the host's lifetime.
class NexusLlmHostHoldReadTest : public ::testing::Test {
 protected:
  NexusLlmHostHoldReadTest()
      : task_environment_(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  base::test::TaskEnvironment task_environment_;
};

TEST_F(NexusLlmHostHoldReadTest,
       OnStreamDoneErasesEntryAfterResolvingHeldRead) {
  NexusLlmHost host(nullptr);
  NexusLlmHost::StreamState state;
  state.stream_id = "stream-1";
  host.streams_.emplace("stream-1", std::move(state));

  bool callback_called = false;
  mojom::NexusLlmStreamReadResultPtr result;
  host.StreamRead(
      "stream-1", /*deadline_ms=*/1e15, /*wait_ms=*/60000.0, base::DictValue(),
      base::BindOnce(
          [](bool* called, mojom::NexusLlmStreamReadResultPtr* out, bool,
             mojom::NexusLlmStreamReadResultPtr res,
             std::optional<std::string>) {
            *called = true;
            *out = std::move(res);
          },
          &callback_called, &result));

  // Queue empty + not done + waitMs present -> the read is HELD, not resolved.
  EXPECT_FALSE(callback_called);
  EXPECT_EQ(host.streams_.count("stream-1"), 1u);

  // Stream finishes while the read is held.
  host.OnStreamDone("stream-1", std::nullopt);

  EXPECT_TRUE(callback_called);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->done);
  ASSERT_TRUE(result->status.has_value());
  EXPECT_EQ(*result->status, "done");
  // Regression: the terminal entry must be erased, not leaked.
  EXPECT_EQ(host.streams_.count("stream-1"), 0u);
}

TEST_F(NexusLlmHostHoldReadTest, HoldTimeoutErasesEntryAndCancelsUpstream) {
  NexusLlmHost host(nullptr);
  NexusLlmHost::StreamState state;
  state.stream_id = "stream-1";
  host.streams_.emplace("stream-1", std::move(state));

  bool callback_called = false;
  mojom::NexusLlmStreamReadResultPtr result;
  host.StreamRead(
      "stream-1", /*deadline_ms=*/1e15, /*wait_ms=*/60000.0, base::DictValue(),
      base::BindOnce(
          [](bool* called, mojom::NexusLlmStreamReadResultPtr* out, bool,
             mojom::NexusLlmStreamReadResultPtr res,
             std::optional<std::string>) {
            *called = true;
            *out = std::move(res);
          },
          &callback_called, &result));

  EXPECT_FALSE(callback_called);
  EXPECT_EQ(host.streams_.count("stream-1"), 1u);

  // Fire the 60s hold timer with no data arriving. GetBroker(nullptr) is a
  // no-op, so the upstream cancel is skipped harmlessly.
  task_environment_.FastForwardBy(base::Seconds(61));

  EXPECT_TRUE(callback_called);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->done);
  ASSERT_TRUE(result->status.has_value());
  EXPECT_EQ(*result->status, "timeout");
  // Regression: the abandoned stream must be erased, not leaked.
  EXPECT_EQ(host.streams_.count("stream-1"), 0u);
}

namespace {

TEST(NexusLlmBridgeSupportTest,
     BuildMojoLlmRequestAcceptsCanonicalToolCallsResultsAndSchemas) {
  base::DictValue payload;

  base::ListValue messages;

  base::DictValue assistant_message;
  assistant_message.Set("role", "assistant");
  base::ListValue assistant_content;
  base::DictValue tool_call;
  tool_call.Set("type", "tool-call");
  tool_call.Set("toolCallId", "call-1");
  tool_call.Set("toolName", "browser_type");
  base::DictValue tool_args;
  tool_args.Set("node_id", 1011);
  tool_args.Set("text", "Peking University official website");
  tool_call.Set("args", std::move(tool_args));
  assistant_content.Append(std::move(tool_call));
  assistant_message.Set("content", std::move(assistant_content));
  messages.Append(std::move(assistant_message));

  base::DictValue tool_message;
  tool_message.Set("role", "tool");
  base::ListValue tool_content;
  base::DictValue tool_result;
  tool_result.Set("type", "tool-result");
  tool_result.Set("toolCallId", "call-1");
  tool_result.Set("toolName", "browser_type");
  base::DictValue result_payload;
  result_payload.Set("ok", true);
  tool_result.Set("result", std::move(result_payload));
  tool_content.Append(std::move(tool_result));
  tool_message.Set("content", std::move(tool_content));
  messages.Append(std::move(tool_message));

  payload.Set("messages", std::move(messages));

  base::ListValue tools;
  base::DictValue tool_definition;
  tool_definition.Set("type", "function");
  tool_definition.Set("name", "browser_type");
  tool_definition.Set("description", "Type into a browser element.");
  base::DictValue parameters;
  parameters.Set("type", "object");
  base::DictValue properties;
  base::DictValue text_property;
  text_property.Set("type", "string");
  properties.Set("text", std::move(text_property));
  parameters.Set("properties", std::move(properties));
  tool_definition.Set("parameters", std::move(parameters));
  tools.Append(std::move(tool_definition));
  payload.Set("tools", std::move(tools));

  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);

  ASSERT_TRUE(request) << error_message;
  EXPECT_TRUE(error_message.empty());

  ASSERT_EQ(request->messages.size(), 2u);
  ASSERT_EQ(request->messages[0]->role, mojom::NexusLlmRole::kAssistant);
  ASSERT_EQ(request->messages[0]->content.size(), 1u);
  EXPECT_EQ(request->messages[0]->content[0]->type,
            mojom::NexusLlmContentPartType::kToolUse);
  EXPECT_EQ(request->messages[0]->content[0]->tool_use_id,
            std::optional<std::string>("call-1"));
  EXPECT_EQ(request->messages[0]->content[0]->name,
            std::optional<std::string>("browser_type"));
  ASSERT_TRUE(request->messages[0]->content[0]->input.has_value());
  const base::DictValue* parsed_args =
      request->messages[0]->content[0]->input->GetIfDict();
  ASSERT_TRUE(parsed_args);
  EXPECT_EQ(parsed_args->FindInt("node_id"), 1011);
  ASSERT_NE(parsed_args->FindString("text"), nullptr);
  EXPECT_EQ(*parsed_args->FindString("text"),
            "Peking University official website");

  ASSERT_EQ(request->messages[1]->role, mojom::NexusLlmRole::kTool);
  ASSERT_EQ(request->messages[1]->content.size(), 1u);
  EXPECT_EQ(request->messages[1]->content[0]->type,
            mojom::NexusLlmContentPartType::kToolResult);
  EXPECT_EQ(request->messages[1]->content[0]->tool_use_id,
            std::optional<std::string>("call-1"));
  EXPECT_EQ(request->messages[1]->content[0]->name,
            std::optional<std::string>("browser_type"));
  ASSERT_TRUE(request->messages[1]->content[0]->result.has_value());
  const base::DictValue* parsed_result =
      request->messages[1]->content[0]->result->GetIfDict();
  ASSERT_TRUE(parsed_result);
  EXPECT_EQ(parsed_result->FindBool("ok"), std::optional<bool>(true));

  ASSERT_EQ(request->tools.size(), 1u);
  EXPECT_EQ(request->tools[0]->name, "browser_type");
  EXPECT_EQ(request->tools[0]->description,
            std::optional<std::string>("Type into a browser element."));
  ASSERT_NE(request->tools[0]->input_schema.FindString("type"), nullptr);
  EXPECT_EQ(*request->tools[0]->input_schema.FindString("type"), "object");
  const base::DictValue* schema_properties =
      request->tools[0]->input_schema.FindDict("properties");
  ASSERT_TRUE(schema_properties);
  const base::DictValue* text_schema = schema_properties->FindDict("text");
  ASSERT_TRUE(text_schema);
  ASSERT_NE(text_schema->FindString("type"), nullptr);
  EXPECT_EQ(*text_schema->FindString("type"), "string");
}

TEST(NexusLlmBridgeSupportTest,
     BuildMojoLlmRequestPreservesMultimodalToolResultParts) {
  base::DictValue payload;
  base::ListValue messages;

  base::DictValue tool_message;
  tool_message.Set("role", "tool");
  base::ListValue tool_content;
  base::DictValue tool_result;
  tool_result.Set("type", "tool-result");
  tool_result.Set("toolCallId", "read-1");
  tool_result.Set("toolName", "Read");

  base::ListValue result_parts;
  base::DictValue text_part;
  text_part.Set("type", "text");
  text_part.Set("text", "Image file read: /tmp/pixel.png");
  result_parts.Append(std::move(text_part));
  base::DictValue image_part;
  image_part.Set("type", "image");
  image_part.Set("image", "aGVsbG8=");
  image_part.Set("mimeType", "image/png");
  result_parts.Append(std::move(image_part));
  base::DictValue doc_part;
  doc_part.Set("type", "document");
  doc_part.Set("data", "JVBERi0=");
  doc_part.Set("mimeType", "application/pdf");
  result_parts.Append(std::move(doc_part));
  tool_result.Set("result", std::move(result_parts));
  tool_content.Append(std::move(tool_result));
  tool_message.Set("content", std::move(tool_content));
  messages.Append(std::move(tool_message));
  payload.Set("messages", std::move(messages));

  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);

  ASSERT_TRUE(request) << error_message;
  ASSERT_EQ(request->messages.size(), 1u);
  ASSERT_EQ(request->messages[0]->role, mojom::NexusLlmRole::kTool);
  ASSERT_EQ(request->messages[0]->content.size(), 1u);
  const auto& part = request->messages[0]->content[0];
  EXPECT_EQ(part->type, mojom::NexusLlmContentPartType::kToolResult);
  EXPECT_EQ(part->tool_use_id, std::optional<std::string>("read-1"));
  ASSERT_TRUE(part->result.has_value());
  const base::ListValue* parsed_parts = part->result->GetIfList();
  ASSERT_TRUE(parsed_parts);
  ASSERT_EQ(parsed_parts->size(), 3u);
  const base::DictValue* parsed_image = (*parsed_parts)[1].GetIfDict();
  ASSERT_TRUE(parsed_image);
  ASSERT_NE(parsed_image->FindString("type"), nullptr);
  EXPECT_EQ(*parsed_image->FindString("type"), "image");
  ASSERT_NE(parsed_image->FindString("mimeType"), nullptr);
  EXPECT_EQ(*parsed_image->FindString("mimeType"), "image/png");
  const base::DictValue* parsed_doc = (*parsed_parts)[2].GetIfDict();
  ASSERT_TRUE(parsed_doc);
  ASSERT_NE(parsed_doc->FindString("type"), nullptr);
  EXPECT_EQ(*parsed_doc->FindString("type"), "document");
  ASSERT_NE(parsed_doc->FindString("mimeType"), nullptr);
  EXPECT_EQ(*parsed_doc->FindString("mimeType"), "application/pdf");
}

// A malformed tool-call part (empty toolCallId — the exact shape the
// qwen/DashScope streaming-parse bug used to persist) must be rejected with a
// graceful error, NOT crash the browser. Bridge payloads carry untrusted
// model-generated + persisted-session content; before the fix the DCHECK(false)
// in FailInvalidBridgeFormat aborted the process on exactly this input.
TEST(NexusLlmBridgeSupportTest,
     BuildMojoLlmRequestRejectsMalformedToolCallWithoutCrashing) {
  base::DictValue payload;
  base::ListValue messages;
  base::DictValue assistant_message;
  assistant_message.Set("role", "assistant");
  base::ListValue content;
  base::DictValue tool_call;
  tool_call.Set("type", "tool-call");
  tool_call.Set("toolCallId", "");  // empty -> invalid
  tool_call.Set("toolName", "Bash");
  tool_call.Set("args", base::DictValue());
  content.Append(std::move(tool_call));
  assistant_message.Set("content", std::move(content));
  messages.Append(std::move(assistant_message));
  payload.Set("messages", std::move(messages));

  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);

  // Graceful rejection: null request + a non-empty error, and no crash.
  EXPECT_FALSE(request);
  EXPECT_FALSE(error_message.empty());
}

// Helper: construct an NexusLlmUsagePtr with the four canonical token fields.
mojom::NexusLlmUsagePtr MakeUsage(int input,
                                  int output,
                                  int cache_read,
                                  int cache_create) {
  auto usage = mojom::NexusLlmUsage::New();
  usage->prompt_tokens = input;
  usage->completion_tokens = output;
  usage->cache_read_input_tokens = cache_read;
  usage->cache_creation_input_tokens = cache_create;
  return usage;
}

TEST(NexusLlmBridgeSupportTest,
     StreamReadResultEmitsMessageStartUsageInSnakeCase) {
  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kMessageStart;
  event->model_id = "claude-opus-4";
  event->usage = MakeUsage(/*input=*/42, /*output=*/0, /*cache_read=*/7,
                           /*cache_create=*/3);

  auto result = mojom::NexusLlmStreamReadResult::New();
  result->events.push_back(std::move(event));
  result->done = false;

  NexusLlmBridgeStreamState state;
  bool done_out = false;
  bool cancel_out = false;
  base::DictValue response =
      StreamReadResultToBridge(&state, *result, &done_out, &cancel_out);

  const base::ListValue* chunks = response.FindList("chunks");
  ASSERT_TRUE(chunks);
  ASSERT_EQ(chunks->size(), 1u);

  const base::DictValue* start = (*chunks)[0].GetIfDict();
  ASSERT_TRUE(start);
  ASSERT_NE(start->FindString("type"), nullptr);
  EXPECT_EQ(*start->FindString("type"), "message_start");

  const base::DictValue* message = start->FindDict("message");
  ASSERT_TRUE(message);
  ASSERT_NE(message->FindString("model"), nullptr);
  EXPECT_EQ(*message->FindString("model"), "claude-opus-4");

  const base::DictValue* usage = message->FindDict("usage");
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->FindInt("input_tokens"), std::optional<int>(42));
  EXPECT_EQ(usage->FindInt("output_tokens"), std::optional<int>(0));
  EXPECT_EQ(usage->FindInt("cache_read_input_tokens"), std::optional<int>(7));
  EXPECT_EQ(usage->FindInt("cache_creation_input_tokens"),
            std::optional<int>(3));

  EXPECT_FALSE(done_out);
  EXPECT_FALSE(cancel_out);
}

TEST(NexusLlmBridgeSupportTest,
     StreamReadResultEmitsTopLevelUsageOnMessageDelta) {
  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kFinish;
  event->finish_reason = mojom::NexusLlmFinishReason::kStop;
  event->usage = MakeUsage(/*input=*/128, /*output=*/64, /*cache_read=*/16,
                           /*cache_create=*/0);

  auto result = mojom::NexusLlmStreamReadResult::New();
  result->events.push_back(std::move(event));
  result->done = true;

  NexusLlmBridgeStreamState state;
  bool done_out = false;
  bool cancel_out = false;
  base::DictValue response =
      StreamReadResultToBridge(&state, *result, &done_out, &cancel_out);

  const base::ListValue* chunks = response.FindList("chunks");
  ASSERT_TRUE(chunks);
  // FinalizeStream emits message_delta (with usage) followed by message_stop.
  ASSERT_GE(chunks->size(), 2u);

  const base::DictValue* delta = nullptr;
  const base::DictValue* stop = nullptr;
  for (const auto& chunk_val : *chunks) {
    const base::DictValue* chunk = chunk_val.GetIfDict();
    ASSERT_TRUE(chunk);
    const std::string* type = chunk->FindString("type");
    ASSERT_TRUE(type);
    if (*type == "message_delta") {
      delta = chunk;
    } else if (*type == "message_stop") {
      stop = chunk;
    }
  }
  ASSERT_TRUE(delta);
  ASSERT_TRUE(stop);

  // Usage is at the top level of the message_delta chunk (Vercel AI SDK
  // LanguageModelV3 wire format), not nested under delta.
  const base::DictValue* usage = delta->FindDict("usage");
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->FindInt("input_tokens"), std::optional<int>(128));
  EXPECT_EQ(usage->FindInt("output_tokens"), std::optional<int>(64));
  EXPECT_EQ(usage->FindInt("cache_read_input_tokens"), std::optional<int>(16));
  EXPECT_EQ(usage->FindInt("cache_creation_input_tokens"),
            std::optional<int>(0));

  const base::DictValue* delta_inner = delta->FindDict("delta");
  ASSERT_TRUE(delta_inner);
  ASSERT_NE(delta_inner->FindString("stop_reason"), nullptr);
  EXPECT_EQ(*delta_inner->FindString("stop_reason"), "end_turn");

  EXPECT_TRUE(done_out);
  EXPECT_FALSE(cancel_out);
}

TEST(NexusLlmBridgeSupportTest,
     StreamReadResultEmitsZeroUsageFieldsWhenFinishHasNoUsage) {
  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kFinish;
  event->finish_reason = mojom::NexusLlmFinishReason::kStop;
  // No usage attached.

  auto result = mojom::NexusLlmStreamReadResult::New();
  result->events.push_back(std::move(event));
  result->done = true;

  NexusLlmBridgeStreamState state;
  bool done_out = false;
  bool cancel_out = false;
  base::DictValue response =
      StreamReadResultToBridge(&state, *result, &done_out, &cancel_out);

  const base::ListValue* chunks = response.FindList("chunks");
  ASSERT_TRUE(chunks);
  // Without usage, FinalizeStream emits only message_delta (finish_reason
  // only) followed by message_stop — no `usage` field should be present.
  const base::DictValue* delta = nullptr;
  for (const auto& chunk_val : *chunks) {
    const base::DictValue* chunk = chunk_val.GetIfDict();
    const std::string* type = chunk ? chunk->FindString("type") : nullptr;
    if (type && *type == "message_delta") {
      delta = chunk;
      break;
    }
  }
  ASSERT_TRUE(delta);
  EXPECT_FALSE(delta->FindDict("usage"));
}

TEST(NexusLlmBridgeSupportTest, CallResultToBridgeEmitsSnakeCaseUsage) {
  auto assistant = mojom::NexusLlmAssistantMessage::New();
  assistant->model_id = "gpt-4o-mini";
  assistant->finish_reason = mojom::NexusLlmFinishReason::kStop;
  assistant->usage = MakeUsage(/*input=*/10, /*output=*/20, /*cache_read=*/5,
                               /*cache_create=*/2);

  auto call_result = mojom::NexusLlmCallResult::New();
  call_result->message = std::move(assistant);

  base::DictValue bridge = CallResultToBridge(*call_result);
  const base::DictValue* message = bridge.FindDict("message");
  ASSERT_TRUE(message);

  const base::DictValue* usage = message->FindDict("usage");
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->FindInt("input_tokens"), std::optional<int>(10));
  EXPECT_EQ(usage->FindInt("output_tokens"), std::optional<int>(20));
  EXPECT_EQ(usage->FindInt("cache_read_input_tokens"), std::optional<int>(5));
  EXPECT_EQ(usage->FindInt("cache_creation_input_tokens"),
            std::optional<int>(2));

  ASSERT_NE(message->FindString("model"), nullptr);
  EXPECT_EQ(*message->FindString("model"), "gpt-4o-mini");
  ASSERT_NE(message->FindString("stop_reason"), nullptr);
  EXPECT_EQ(*message->FindString("stop_reason"), "end_turn");
}

TEST(NexusLlmBridgeSupportTest, BuildMojoLlmRequestRejectsLegacyToolFormats) {
  base::DictValue payload;

  base::ListValue messages;
  base::DictValue assistant_message;
  assistant_message.Set("role", "assistant");
  base::ListValue assistant_content;
  base::DictValue legacy_tool_use;
  legacy_tool_use.Set("type", "tool_use");
  legacy_tool_use.Set("id", "call-1");
  legacy_tool_use.Set("name", "browser_type");
  legacy_tool_use.Set("input", base::DictValue());
  assistant_content.Append(std::move(legacy_tool_use));
  assistant_message.Set("content", std::move(assistant_content));
  messages.Append(std::move(assistant_message));
  payload.Set("messages", std::move(messages));

  base::ListValue tools;
  base::DictValue legacy_tool_definition;
  legacy_tool_definition.Set("type", "function");
  legacy_tool_definition.Set("name", "browser_type");
  base::DictValue input_schema;
  input_schema.Set("type", "object");
  legacy_tool_definition.Set("input_schema", std::move(input_schema));
  tools.Append(std::move(legacy_tool_definition));
  payload.Set("tools", std::move(tools));

  EXPECT_DCHECK_DEATH({
    std::string error_message;
    auto request = BuildMojoLlmRequest(payload, &error_message);
    (void)request;
  });
}

}  // namespace
}  // namespace nexus
