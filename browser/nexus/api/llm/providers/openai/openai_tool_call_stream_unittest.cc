// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_call_stream.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Wraps a string literal into an optional string for the MakeToolCall args
// below (avoids the double user-defined conversion const char* -> string ->
// optional<string>).
std::optional<std::string> S(const char* s) {
  return std::string(s);
}

// Builds one `delta.tool_calls[i]` entry. `id` / `name` are optional so a test
// can model both dialects: OpenAI OMITS them on follow-up deltas (nullopt),
// while DashScope (Qwen) PRESENTS them with an empty id + null name
// (S("") / nullopt).
base::DictValue MakeToolCall(int index,
                             const std::optional<std::string>& id,
                             const std::optional<std::string>& name,
                             const std::string& args) {
  base::DictValue tool_call;
  tool_call.Set("index", index);
  if (id.has_value())
    tool_call.Set("id", *id);
  base::DictValue fn;
  if (name.has_value())
    fn.Set("name", *name);
  fn.Set("arguments", args);
  tool_call.Set("function", std::move(fn));
  return tool_call;
}

// Collects every emitted chunk so a test can assert on the exact sequence.
class ChunkCollector {
 public:
  base::RepeatingCallback<void(base::DictValue)> callback() {
    return base::BindRepeating(&ChunkCollector::OnChunk,
                               base::Unretained(this));
  }

  const std::vector<base::DictValue>& chunks() const { return chunks_; }

  int CountStarts() const {
    int count = 0;
    for (const auto& chunk : chunks_) {
      if (const std::string* type = chunk.FindString("type");
          type && *type == "tool-call-streaming-start") {
        ++count;
      }
    }
    return count;
  }

  // Concatenates the argsTextDelta of every tool-call-delta for `tool_call_id`.
  std::string ArgsFor(const std::string& tool_call_id) const {
    std::string args;
    for (const auto& chunk : chunks_) {
      const std::string* type = chunk.FindString("type");
      const std::string* id = chunk.FindString("toolCallId");
      if (type && *type == "tool-call-delta" && id && *id == tool_call_id) {
        if (const std::string* delta = chunk.FindString("argsTextDelta"))
          args += *delta;
      }
    }
    return args;
  }

 private:
  void OnChunk(base::DictValue chunk) { chunks_.push_back(std::move(chunk)); }

  std::vector<base::DictValue> chunks_;
};

// DashScope (Qwen) repeats the tool-call object on EVERY argument delta but
// with an empty `id` ("") and null `function.name` after the first chunk. The
// translation must open exactly ONE well-formed tool call and keep correlating
// every argument delta to the real id. Without the fix this emits one start
// per chunk (each nameless) and blanks the id, so the args never assemble.
TEST(OpenAiToolCallStreamTest, DashScopeEmptyIdRepeatOpensSingleToolCall) {
  ChunkCollector collector;
  std::map<int, std::string> ids;
  auto emit = collector.callback();

  // Opening chunk: real id + name, empty arguments.
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S("call_qwen_1"), S("Bash"), ""), ids, emit);
  // Follow-up chunks: DashScope repeats `id` as "" and drops the name.
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S(""), std::nullopt, "{\"command\":"), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S(""), std::nullopt, "\"git status\"}"), ids, emit);

  // Exactly one start, carrying the real id + name.
  ASSERT_EQ(collector.CountStarts(), 1);
  const base::DictValue& start = collector.chunks().front();
  EXPECT_EQ(*start.FindString("toolCallId"), "call_qwen_1");
  EXPECT_EQ(*start.FindString("toolName"), "Bash");

  // Every argument delta still correlates to the real id (never blanked).
  for (const auto& chunk : collector.chunks()) {
    if (const std::string* type = chunk.FindString("type");
        type && *type == "tool-call-delta") {
      const std::string* id = chunk.FindString("toolCallId");
      ASSERT_NE(id, nullptr);
      EXPECT_EQ(*id, "call_qwen_1");
    }
  }
  EXPECT_EQ(collector.ArgsFor("call_qwen_1"), "{\"command\":\"git status\"}");
}

// OpenAI omits `id` / `function.name` on follow-up deltas entirely. The
// standard path must keep working: one start, args assembled by index.
TEST(OpenAiToolCallStreamTest, OpenAiStyleOmittedIdStillOpensOnce) {
  ChunkCollector collector;
  std::map<int, std::string> ids;
  auto emit = collector.callback();

  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S("call_o1"), S("get_weather"), ""), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, std::nullopt, std::nullopt, "{\"loc\":"), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, std::nullopt, std::nullopt, "\"NYC\"}"), ids, emit);

  ASSERT_EQ(collector.CountStarts(), 1);
  EXPECT_EQ(*collector.chunks().front().FindString("toolName"), "get_weather");
  EXPECT_EQ(collector.ArgsFor("call_o1"), "{\"loc\":\"NYC\"}");
}

// A provider that repeats a NON-empty id must not re-open the call either.
TEST(OpenAiToolCallStreamTest, RepeatedNonEmptyIdDoesNotReopen) {
  ChunkCollector collector;
  std::map<int, std::string> ids;
  auto emit = collector.callback();

  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S("call_x"), S("Bash"), ""), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S("call_x"), S("Bash"), "{\"a\":1}"), ids, emit);

  EXPECT_EQ(collector.CountStarts(), 1);
  EXPECT_EQ(collector.ArgsFor("call_x"), "{\"a\":1}");
}

// Parallel tool calls (distinct indices) each open once and their argument
// deltas correlate to the right id.
TEST(OpenAiToolCallStreamTest, ParallelToolCallsCorrelateByIndex) {
  ChunkCollector collector;
  std::map<int, std::string> ids;
  auto emit = collector.callback();

  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S("call_a"), S("Bash"), ""), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(1, S("call_b"), S("Read"), ""), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(0, S(""), std::nullopt, "{\"a\":1}"), ids, emit);
  TranslateChatCompletionToolCallDelta(
      MakeToolCall(1, S(""), std::nullopt, "{\"b\":2}"), ids, emit);

  EXPECT_EQ(collector.CountStarts(), 2);
  EXPECT_EQ(collector.ArgsFor("call_a"), "{\"a\":1}");
  EXPECT_EQ(collector.ArgsFor("call_b"), "{\"b\":2}");
}

}  // namespace
}  // namespace nexus
