// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_stop_reason.h"

#include <array>
#include <string_view>

namespace nexus {

FinishReason TranslateAnthropicStopReason(std::string_view stop_reason) {
  if (stop_reason == "end_turn" || stop_reason == "stop_sequence")
    return FinishReason::kStop;
  if (stop_reason == "max_tokens")
    return FinishReason::kLength;
  if (stop_reason == "tool_use")
    return FinishReason::kToolCalls;
  if (stop_reason == "refusal")
    return FinishReason::kContentFilter;
  return FinishReason::kOther;
}

std::optional<base::DictValue> NormalizeAnthropicStopDetails(
    const base::Value* stop_details) {
  if (!stop_details)
    return std::nullopt;
  const base::DictValue* dict = stop_details->GetIfDict();
  if (!dict)
    return std::nullopt;

  static constexpr std::array<std::string_view, 4> kStringFields = {
      "type", "category", "explanation", "recommended_model"};

  base::DictValue out;
  for (std::string_view field : kStringFields) {
    if (const std::string* value = dict->FindString(field);
        value && !value->empty()) {
      out.Set(field, *value);
    }
  }
  if (out.empty())
    return std::nullopt;
  return out;
}

}  // namespace nexus
