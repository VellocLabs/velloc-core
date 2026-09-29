// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_support.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"

#include <string>
#include <vector>

#include "base/test/bind.h"
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

// A terminal in-stream error must cancel the upstream provider request. The
// provider can report the error on a still-open HTTP 200 stream (the Codex
// Responses backend does); without the cancel the SSE loader keeps its
// concurrency slot until the provider closes the body or the idle guard
// fires, and under max_concurrent_streams:1 (chatgpt-pro) the guest's retry
// queues behind that corpse. The hook mirrors the real providers' hazard:
// OpenAiProvider::CancelRequest runs the stream's done callback SYNCHRONOUSLY
// with "Cancelled", so the re-entry into OnStreamDone must neither replace
// the provider's own error message nor touch erased state.
TEST(NexusLlmHostTest, StreamErrorChunkCancelsUpstreamRequest) {
  NexusLlmHost host(nullptr);
  NexusLlmHost::StreamState state;
  state.stream_id = "stream-1";
  host.streams_.emplace("stream-1", std::move(state));

  std::vector<std::string> cancelled;
  host.SetUpstreamCancelHookForTesting(
      base::BindLambdaForTesting([&](const std::string& request_id) {
        cancelled.push_back(request_id);
        host.OnStreamDone(request_id, "Cancelled");
      }));

  base::DictValue chunk;
  chunk.Set("type", "error");
  base::DictValue error;
  error.Set("message", "HTTP 503: provider fell over");
  chunk.Set("error", std::move(error));
  host.OnStreamChunk("stream-1", std::move(chunk));

  // The error branch cancelled upstream, exactly once, for this stream.
  ASSERT_EQ(cancelled.size(), 1u);
  EXPECT_EQ(cancelled[0], "stream-1");

  // A late (polling-guest) read still drains the PROVIDER's error message —
  // the synchronous "Cancelled" re-entry did not replace it.
  bool callback_called = false;
  mojom::NexusLlmStreamReadResultPtr result;
  host.StreamRead(
      "stream-1", /*deadline_ms=*/std::nullopt, /*wait_ms=*/std::nullopt,
      base::DictValue(),
      base::BindOnce(
          [](bool* called, mojom::NexusLlmStreamReadResultPtr* out, bool,
             mojom::NexusLlmStreamReadResultPtr res,
             std::optional<std::string>) {
            *called = true;
            *out = std::move(res);
          },
          &callback_called, &result));

  EXPECT_TRUE(callback_called);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->done);
  ASSERT_EQ(result->events.size(), 1u);
  ASSERT_TRUE(result->events[0]);
  EXPECT_EQ(result->events[0]->type, mojom::NexusLlmStreamEventType::kError);
  EXPECT_EQ(result->events[0]->error_message,
            std::optional<std::string>("HTTP 503: provider fell over"));
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

  // ResolveHeldRead POSTS the callback rather than running it inline (it
  // breaks the adapter-read → Cancel → erase-my-own-state re-entrancy loop),
  // so the resolution lands on the next task-runner drain, not synchronously.
  task_environment_.RunUntilIdle();

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

TEST_F(NexusLlmHostHoldReadTest,
       StreamErrorCancelsUpstreamAndResolvesHeldRead) {
  // The held-read variant of StreamErrorChunkCancelsUpstreamRequest: the
  // error chunk resolves the held read (which ERASES the stream entry), and
  // only then cancels upstream. With the cancel ordered before the erase,
  // the hook's synchronous OnStreamDone("Cancelled") re-entry would operate
  // on — and this function would then use — a dangling StreamState.
  NexusLlmHost host(nullptr);
  NexusLlmHost::StreamState state;
  state.stream_id = "stream-1";
  host.streams_.emplace("stream-1", std::move(state));

  int cancels = 0;
  host.SetUpstreamCancelHookForTesting(
      base::BindLambdaForTesting([&](const std::string& request_id) {
        ++cancels;
        // By the time the host cancels, the entry is already erased — the
        // re-entry must find nothing and change nothing.
        EXPECT_EQ(host.streams_.count(request_id), 0u);
        host.OnStreamDone(request_id, "Cancelled");
      }));

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

  base::DictValue chunk;
  chunk.Set("type", "error");
  base::DictValue error;
  error.Set("message", "HTTP 503: provider fell over");
  chunk.Set("error", std::move(error));
  host.OnStreamChunk("stream-1", std::move(chunk));

  // The cancel is synchronous with the error branch — it must not wait on a
  // task drain, or the upstream request outlives the failed attempt exactly
  // as it did before this fix.
  EXPECT_EQ(cancels, 1);

  // The held read's resolution is POSTED (see ResolveHeldRead), so drain.
  task_environment_.RunUntilIdle();

  // The held read resolved with the provider's error, and the entry is gone.
  EXPECT_TRUE(callback_called);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->done);
  ASSERT_EQ(result->events.size(), 1u);
  ASSERT_TRUE(result->events[0]);
  EXPECT_EQ(result->events[0]->type, mojom::NexusLlmStreamEventType::kError);
  EXPECT_EQ(result->events[0]->error_message,
            std::optional<std::string>("HTTP 503: provider fell over"));
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

// An @-mentioned PDF is inlined by the guest as a TOP-LEVEL `document` part
// of a user message. The bridge used to accept a document only inside a tool
// result and rejected this one with INVALID_ARGS, failing the whole turn.
TEST(NexusLlmBridgeSupportTest, BuildMojoLlmRequestAcceptsTopLevelDocumentPart) {
  base::DictValue payload;
  base::ListValue messages;
  base::DictValue user_message;
  user_message.Set("role", "user");
  base::ListValue content;
  base::DictValue text_part;
  text_part.Set("type", "text");
  text_part.Set("text", "PDF file read: /tmp/claim.pdf (2 page(s))");
  content.Append(std::move(text_part));
  base::DictValue doc_part;
  doc_part.Set("type", "document");
  doc_part.Set("data", "JVBERi0=");
  doc_part.Set("mimeType", "application/pdf");
  content.Append(std::move(doc_part));
  user_message.Set("content", std::move(content));
  messages.Append(std::move(user_message));
  payload.Set("messages", std::move(messages));

  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);

  ASSERT_TRUE(request) << error_message;
  ASSERT_EQ(request->messages.size(), 1u);
  ASSERT_EQ(request->messages[0]->content.size(), 2u);
  const auto& part = request->messages[0]->content[1];
  EXPECT_EQ(part->type, mojom::NexusLlmContentPartType::kDocument);
  EXPECT_EQ(part->data, std::optional<std::string>("JVBERi0="));
  EXPECT_EQ(part->mime_type, std::optional<std::string>("application/pdf"));
}

namespace {

base::DictValue PayloadWithOneUserMessageAndTools(base::ListValue tools) {
  base::DictValue payload;
  base::ListValue messages;
  base::DictValue user_message;
  user_message.Set("role", "user");
  user_message.Set("content", "hi");
  messages.Append(std::move(user_message));
  payload.Set("messages", std::move(messages));
  payload.Set("tools", std::move(tools));
  return payload;
}

}  // namespace

// The guest sends every tool as a full definition; the host no longer
// resolves `{ ref: name }` entries from a catalog/registry. A stray ref is a
// malformed request, not a silently dropped (or silently resolved) tool.
TEST(NexusLlmBridgeSupportTest, BuildMojoLlmRequestRejectsToolRefEntries) {
  base::ListValue tools;
  base::DictValue ref;
  ref.Set("ref", "Bash");
  tools.Append(std::move(ref));
  base::DictValue payload = PayloadWithOneUserMessageAndTools(std::move(tools));

  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);

  EXPECT_FALSE(request);
  EXPECT_NE(error_message.find("type=\"function\""), std::string::npos)
      << error_message;
}

// A full definition reaches the provider exactly as sent: the schema the
// guest chose (strict or regular) and its `strict` flag. The payload-level
// `strictTools` switch that used to pick a variant for refs is ignored.
TEST(NexusLlmBridgeSupportTest, BuildMojoLlmRequestForwardsFullToolDefinition) {
  base::ListValue tools;
  base::DictValue def;
  def.Set("type", "function");
  def.Set("name", "Read");
  def.Set("description", "Read a file");
  base::DictValue parameters;
  parameters.Set("type", "object");
  parameters.Set("additionalProperties", false);
  def.Set("parameters", std::move(parameters));
  def.Set("strict", true);
  tools.Append(std::move(def));
  base::DictValue payload = PayloadWithOneUserMessageAndTools(std::move(tools));
  payload.Set("strictTools", false);

  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);

  ASSERT_TRUE(request) << error_message;
  ASSERT_EQ(request->tools.size(), 1u);
  const auto& tool = request->tools[0];
  EXPECT_EQ(tool->name, "Read");
  EXPECT_EQ(tool->description, "Read a file");
  EXPECT_TRUE(tool->strict);
  EXPECT_EQ(tool->input_schema.FindBool("additionalProperties"),
            std::optional<bool>(false));
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

// A content-filter finish (Anthropic `stop_reason: "refusal"`) carries the
// provider's `stop_details` beside `stop_reason` on the blocking-call reply,
// in the vendor's own field name so the guest reads one shape for both
// transports. Absent entirely when the provider reported none.
TEST(NexusLlmBridgeSupportTest, CallResultToBridgeCarriesStopDetails) {
  auto assistant = mojom::NexusLlmAssistantMessage::New();
  assistant->model_id = "claude-fable-5-1";
  assistant->finish_reason = mojom::NexusLlmFinishReason::kContentFilter;
  assistant->usage = MakeUsage(/*input=*/10, /*output=*/0, /*cache_read=*/0,
                               /*cache_create=*/0);
  base::DictValue details;
  details.Set("type", "refusal");
  details.Set("category", "cyber");
  details.Set("explanation", "Declined by a safety classifier.");
  assistant->stop_details = std::move(details);

  auto call_result = mojom::NexusLlmCallResult::New();
  call_result->message = std::move(assistant);

  base::DictValue bridge = CallResultToBridge(*call_result);
  const base::DictValue* message = bridge.FindDict("message");
  ASSERT_TRUE(message);
  ASSERT_NE(message->FindString("stop_reason"), nullptr);
  EXPECT_EQ(*message->FindString("stop_reason"), "content_filter");
  const base::DictValue* stop_details = message->FindDict("stop_details");
  ASSERT_TRUE(stop_details);
  ASSERT_NE(stop_details->FindString("category"), nullptr);
  EXPECT_EQ(*stop_details->FindString("category"), "cyber");
  ASSERT_NE(stop_details->FindString("explanation"), nullptr);
  EXPECT_EQ(*stop_details->FindString("explanation"),
            "Declined by a safety classifier.");

  // No details reported -> no key at all (the guest treats absence as "the
  // provider said nothing more", never as an empty object).
  auto plain = mojom::NexusLlmAssistantMessage::New();
  plain->finish_reason = mojom::NexusLlmFinishReason::kStop;
  plain->usage = MakeUsage(1, 1, 0, 0);
  auto plain_result = mojom::NexusLlmCallResult::New();
  plain_result->message = std::move(plain);
  base::DictValue plain_bridge = CallResultToBridge(*plain_result);
  const base::DictValue* plain_message = plain_bridge.FindDict("message");
  ASSERT_TRUE(plain_message);
  EXPECT_EQ(plain_message->FindDict("stop_details"), nullptr);
}

// Streaming twin of the above: the kFinish event's `stop_details` lands on
// `message_delta.delta.stop_details`, next to the normalized `stop_reason`,
// which is where the Anthropic SSE itself puts it.
TEST(NexusLlmBridgeSupportTest,
     StreamReadResultToBridgeEmitsStopDetailsOnMessageDelta) {
  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kFinish;
  event->finish_reason = mojom::NexusLlmFinishReason::kContentFilter;
  event->usage = MakeUsage(/*input=*/10, /*output=*/0, /*cache_read=*/0,
                           /*cache_create=*/0);
  base::DictValue details;
  details.Set("type", "refusal");
  details.Set("category", "bio");
  event->stop_details = std::move(details);

  auto read_result = mojom::NexusLlmStreamReadResult::New();
  read_result->events.push_back(std::move(event));
  read_result->done = true;

  NexusLlmBridgeStreamState state;
  bool done = false;
  bool should_cancel = false;
  base::DictValue bridge =
      StreamReadResultToBridge(&state, *read_result, &done, &should_cancel);

  const base::ListValue* chunks = bridge.FindList("chunks");
  ASSERT_TRUE(chunks);
  const base::DictValue* message_delta = nullptr;
  for (const base::Value& chunk : *chunks) {
    const base::DictValue* dict = chunk.GetIfDict();
    if (dict && dict->FindString("type") &&
        *dict->FindString("type") == "message_delta") {
      message_delta = dict;
    }
  }
  ASSERT_TRUE(message_delta);
  const base::DictValue* delta = message_delta->FindDict("delta");
  ASSERT_TRUE(delta);
  ASSERT_NE(delta->FindString("stop_reason"), nullptr);
  EXPECT_EQ(*delta->FindString("stop_reason"), "content_filter");
  const base::DictValue* stop_details = delta->FindDict("stop_details");
  ASSERT_TRUE(stop_details);
  ASSERT_NE(stop_details->FindString("category"), nullptr);
  EXPECT_EQ(*stop_details->FindString("category"), "bio");
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
