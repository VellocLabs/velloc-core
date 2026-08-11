// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_responses_error.h"

#include <string_view>

#include "base/json/json_writer.h"
#include "base/strings/string_util.h"

namespace nexus {

namespace {

const std::string* FindNonEmptyString(const base::DictValue& dict,
                                      std::string_view key) {
  const std::string* value = dict.FindString(key);
  return (value && !value->empty()) ? value : nullptr;
}

const std::string* FindNestedNonEmptyString(const base::DictValue& dict,
                                            std::string_view outer,
                                            std::string_view inner) {
  const base::DictValue* sub = dict.FindDict(outer);
  return sub ? FindNonEmptyString(*sub, inner) : nullptr;
}

// The only keys worth keeping from a `response.failed` event's `response`
// object. Everything else echoes the REQUEST back: `instructions` is the
// system prompt this provider sends (BuildResponsesApiRequestBody), and
// `input` / `output` carry the conversation itself. That payload must not be
// serialized — this string is written to chrome_debug.log on every failed
// stream, and for an unrecognized shape it also becomes visible transcript
// text. `base::Value::Dict` is key-sorted, so `instructions` sorts early and
// would otherwise spend the whole byte budget printing the user's prompt.
constexpr std::string_view kResponseDiagnosticKeys[] = {
    "error", "id", "incomplete_details", "status"};

}  // namespace

std::string SerializeEventForDiagnosis(const base::DictValue& data) {
  // Prune before serializing — see kResponseDiagnosticKeys. A bare `error`
  // event carries only error fields and is kept whole.
  base::DictValue pruned = data.Clone();
  if (const base::DictValue* response_obj = data.FindDict("response")) {
    base::DictValue kept;
    for (std::string_view key : kResponseDiagnosticKeys) {
      if (const base::Value* value = response_obj->Find(key))
        kept.Set(key, value->Clone());
    }
    pruned.Set("response", std::move(kept));
  }

  std::string serialized;
  if (!base::JSONWriter::Write(pruned, &serialized) || serialized.empty())
    return "<unserializable event>";
  if (serialized.size() > kMaxSerializedErrorEventChars) {
    // Truncate on a CODEPOINT boundary, not a byte offset. base::JSONWriter
    // never escapes non-ASCII — EscapeJSONStringImpl escapes only control
    // characters and a few specials, everything else is written as raw UTF-8
    // — and a provider error message can be localized, so a plain resize()
    // can split a multi-byte sequence. On the unrecognized path this string
    // becomes a base::Value (`chunk.Set("error", …)`), whose std::string
    // constructor DCHECKs `IsStringUTF8AllowingNoncharacters`: in this
    // is_debug tree a split sequence aborts the browser process. It then
    // travels to the guest as `NexusLlmStreamEvent.error_message`, a mojo
    // `string?`, which also requires valid UTF-8.
    serialized = std::string(base::TruncateUTF8ToByteSize(
        serialized, kMaxSerializedErrorEventChars));
    serialized += "...(truncated)";
  }
  return serialized;
}

std::string ExtractResponsesApiErrorMessage(const base::DictValue& data) {
  // `response.failed` — the documented streaming shape.
  if (const base::DictValue* response_obj = data.FindDict("response")) {
    if (const std::string* m =
            FindNestedNonEmptyString(*response_obj, "error", "message")) {
      return *m;
    }
  }
  // OpenAI's non-streaming envelope, reused on an SSE `error` event by the
  // Codex backend.
  if (const std::string* m = FindNestedNonEmptyString(data, "error", "message"))
    return *m;
  // The documented bare `error` event.
  if (const std::string* m = FindNonEmptyString(data, "message"))
    return *m;
  // `error` delivered as a bare string rather than an object.
  if (const std::string* m = FindNonEmptyString(data, "error"))
    return *m;
  return std::string();
}

std::string ExtractResponsesApiErrorCode(const base::DictValue& data) {
  static constexpr std::string_view kCodeKeys[] = {"code", "type"};
  // `type` is only meaningful INSIDE an error object — the top-level `type` of
  // a bare `error` event is the literal string "error", which is not a code.
  const base::DictValue* response_obj = data.FindDict("response");
  for (const base::DictValue* err :
       {response_obj ? response_obj->FindDict("error") : nullptr,
        data.FindDict("error")}) {
    if (!err)
      continue;
    for (std::string_view key : kCodeKeys) {
      if (const std::string* code = FindNonEmptyString(*err, key))
        return *code;
    }
  }
  if (const std::string* code = FindNonEmptyString(data, "code"))
    return *code;
  return std::string();
}

std::string BuildResponsesApiErrorMessage(const base::DictValue& data,
                                          bool* out_recognized) {
  std::string message = ExtractResponsesApiErrorMessage(data);
  const bool recognized = !message.empty();
  if (out_recognized)
    *out_recognized = recognized;

  if (!recognized) {
    // Unknown envelope. Keep the event verbatim: a placeholder tells neither
    // the user nor the guest's classifier anything, and an unclassifiable
    // message is treated as non-retryable.
    return "unrecognized error event: " + SerializeEventForDiagnosis(data);
  }

  const std::string code = ExtractResponsesApiErrorCode(data);
  if (!code.empty() && message.find(code) == std::string::npos)
    return code + ": " + message;
  return message;
}

}  // namespace nexus
