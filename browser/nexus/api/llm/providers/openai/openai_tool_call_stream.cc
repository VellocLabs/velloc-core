// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_call_stream.h"

#include <utility>

namespace nexus {

void TranslateChatCompletionToolCallDelta(
    const base::DictValue& tool_call,
    std::map<int, std::string>& tool_call_ids,
    const base::RepeatingCallback<void(base::DictValue)>& emit) {
  const int index = tool_call.FindInt("index").value_or(0);

  // Open the tool call exactly once per index, and only for a NON-EMPTY id.
  // DashScope (Qwen) repeats the tool-call object on every argument delta with
  // an empty `id` ("") and null name; `FindString` returns non-null for "", so
  // both the non-empty check and the once-per-index check are needed to avoid
  // re-emitting the start and blanking the correlated id (see the header for
  // the full rationale).
  const std::string* id = tool_call.FindString("id");
  if (id && !id->empty() && tool_call_ids.find(index) == tool_call_ids.end()) {
    tool_call_ids[index] = *id;

    base::DictValue chunk;
    chunk.Set("type", "tool-call-streaming-start");
    chunk.Set("toolCallId", *id);
    chunk.Set("toolCallType", "function");
    if (const base::DictValue* fn = tool_call.FindDict("function")) {
      if (const std::string* name = fn->FindString("name"))
        chunk.Set("toolName", *name);
    }
    emit.Run(std::move(chunk));
  }

  if (const base::DictValue* fn = tool_call.FindDict("function")) {
    if (const std::string* args = fn->FindString("arguments")) {
      if (!args->empty()) {
        base::DictValue chunk;
        chunk.Set("type", "tool-call-delta");
        auto id_it = tool_call_ids.find(index);
        if (id_it != tool_call_ids.end())
          chunk.Set("toolCallId", id_it->second);
        chunk.Set("argsTextDelta", *args);
        emit.Run(std::move(chunk));
      }
    }
  }
}

}  // namespace nexus
