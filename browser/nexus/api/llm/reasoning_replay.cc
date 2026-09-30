// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/reasoning_replay.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace nexus {

std::optional<std::string> EncodeReasoningReplaySignature(
    std::string_view provider_id,
    std::string_view model_id,
    const base::DictValue& replay_part) {
  const std::string* field = replay_part.FindString("field");
  const base::Value* value = replay_part.Find("value");
  if (!field || field->empty() || !value || value->is_none() ||
      provider_id.empty() || model_id.empty()) {
    return std::nullopt;
  }

  base::DictValue envelope;
  envelope.Set("provider", provider_id);
  envelope.Set("model", model_id);
  envelope.Set("field", *field);
  envelope.Set("value", value->Clone());

  std::string json;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &json))
    return std::nullopt;
  return std::string(kReasoningReplayPrefix) + json;
}

std::optional<base::DictValue> DecodeReasoningReplaySignatureFor(
    std::string_view signature,
    std::string_view provider_id,
    std::string_view model_id) {
  if (!base::StartsWith(signature, kReasoningReplayPrefix))
    return std::nullopt;

  std::optional<base::DictValue> envelope = base::JSONReader::ReadDict(
      signature.substr(kReasoningReplayPrefix.size()), base::JSON_PARSE_RFC);
  if (!envelope)
    return std::nullopt;

  const std::string* provider = envelope->FindString("provider");
  const std::string* model = envelope->FindString("model");
  const std::string* field = envelope->FindString("field");
  base::Value* value = envelope->Find("value");
  if (!provider || *provider != provider_id || !model || *model != model_id ||
      !field || field->empty() || !value || value->is_none()) {
    return std::nullopt;
  }

  base::DictValue part;
  part.Set("type", kReasoningReplayPartType);
  part.Set("field", *field);
  part.Set("value", std::move(*value));
  return part;
}

}  // namespace nexus
