// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_reasoning.h"

#include <string>
#include <string_view>

#include "base/values.h"

namespace nexus {

namespace {

// Joins the non-empty `field` strings of every dict in `parts` whose `type`
// is `part_type`, separated by `separator`.
std::string JoinParts(const base::ListValue* parts,
                      const char* part_type,
                      const char* field,
                      const char* separator) {
  std::string out;
  if (!parts)
    return out;
  for (const auto& part_val : *parts) {
    const base::DictValue* part = part_val.GetIfDict();
    if (!part)
      continue;
    const std::string* type = part->FindString("type");
    if (!type || *type != part_type)
      continue;
    const std::string* text = part->FindString(field);
    if (!text || text->empty())
      continue;
    if (!out.empty())
      out += separator;
    out += *text;
  }
  return out;
}

}  // namespace

std::string ExtractChatCompletionReasoningText(const base::DictValue& delta) {
  for (const char* key : {"reasoning_content", "reasoning"}) {
    if (const std::string* text = delta.FindString(key);
        text && !text->empty()) {
      return *text;
    }
  }

  const base::ListValue* details = delta.FindList("reasoning_details");
  if (!details)
    return std::string();
  std::string out;
  for (const auto& detail_val : *details) {
    const base::DictValue* detail = detail_val.GetIfDict();
    if (!detail)
      continue;
    const std::string* type = detail->FindString("type");
    if (!type)
      continue;
    const std::string* text = nullptr;
    if (*type == "reasoning.text")
      text = detail->FindString("text");
    else if (*type == "reasoning.summary")
      text = detail->FindString("summary");
    if (text)
      out += *text;
  }
  return out;
}

std::string ExtractResponsesReasoningItemText(const base::DictValue& item) {
  std::string summary =
      JoinParts(item.FindList("summary"), "summary_text", "text", "\n\n");
  if (!summary.empty())
    return summary;
  return JoinParts(item.FindList("content"), "reasoning_text", "text",
                   "\n\n");
}

bool IsResponsesReasoningTextDeltaEvent(std::string_view event_type) {
  return event_type == "response.reasoning_summary_text.delta" ||
         event_type == "response.reasoning_text.delta";
}

}  // namespace nexus
