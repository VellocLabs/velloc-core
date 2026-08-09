#include "custom_browser/browser/nexus/nexus_bridge_support.h"

#include <algorithm>
#include <cmath>

#include "base/containers/span.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"

namespace nexus::bridge_support {

std::string SerializeJson(base::Value value) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  return json;
}

blink::WebMessagePayload Utf8Payload(std::string_view utf8) {
  return blink::WebMessagePayload(base::UTF8ToUTF16(utf8));
}

std::optional<std::string> PayloadToUtf8(const blink::WebMessagePayload& payload) {
  const auto* text = std::get_if<std::u16string>(&payload);
  if (!text) {
    return std::nullopt;
  }
  return base::UTF16ToUTF8(*text);
}

bool IsBinaryPayload(const blink::WebMessagePayload& payload) {
  return std::holds_alternative<
      std::unique_ptr<blink::WebMessageArrayBufferPayload>>(payload);
}

std::vector<uint8_t> PayloadToBytes(const blink::WebMessagePayload& payload) {
  const auto* binary =
      std::get_if<std::unique_ptr<blink::WebMessageArrayBufferPayload>>(
          &payload);
  if (!binary || !binary->get()) {
    return {};
  }

  if (std::optional<base::span<const uint8_t>> span =
          (*binary)->GetAsSpanIfPossible()) {
    return std::vector<uint8_t>(span->begin(), span->end());
  }

  std::vector<uint8_t> bytes((*binary)->GetLength());
  (*binary)->CopyInto(base::span(bytes));
  return bytes;
}

blink::WebMessagePayload BigBufferToPayload(mojo_base::BigBuffer buffer) {
  return blink::WebMessagePayload(
      blink::WebMessageArrayBufferPayload::CreateFromBigBuffer(
          std::move(buffer), std::nullopt));
}

std::vector<uint8_t> ValueListToBytes(const base::ListValue* list) {
  std::vector<uint8_t> bytes;
  if (!list) {
    return bytes;
  }

  bytes.reserve(list->size());
  for (const base::Value& value : *list) {
    int int_value = 0;
    if (std::optional<int> maybe_int = value.GetIfInt()) {
      int_value = *maybe_int;
    } else if (std::optional<double> maybe_double = value.GetIfDouble();
               maybe_double && std::isfinite(*maybe_double)) {
      int_value = static_cast<int>(*maybe_double);
    }
    bytes.push_back(static_cast<uint8_t>(std::clamp(int_value, 0, 255)));
  }
  return bytes;
}

std::optional<std::string> FindNonEmptyString(const base::DictValue& dict,
                                              std::string_view key) {
  const std::string* value = dict.FindString(key);
  if (!value || value->empty()) {
    return std::nullopt;
  }
  return *value;
}

std::optional<int64_t> FindIntegralValue(const base::DictValue& dict,
                                         std::string_view key) {
  if (std::optional<int> value = dict.FindInt(key)) {
    return static_cast<int64_t>(*value);
  }
  if (std::optional<double> value = dict.FindDouble(key);
      value && std::isfinite(*value) && std::floor(*value) == *value) {
    return static_cast<int64_t>(*value);
  }
  // LLMs sometimes emit integers as strings (e.g. "25" instead of 25).
  if (const std::string* str = dict.FindString(key)) {
    int parsed = 0;
    if (base::StringToInt(*str, &parsed)) {
      return static_cast<int64_t>(parsed);
    }
  }
  return std::nullopt;
}

bool CopyRequiredString(const base::DictValue& dict,
                        std::string_view key,
                        std::string* out,
                        std::string* error_message) {
  std::optional<std::string> value = FindNonEmptyString(dict, key);
  if (!value) {
    *error_message = "Missing string field \"" + std::string(key) + "\"";
    return false;
  }
  *out = *value;
  return true;
}

}  // namespace nexus::bridge_support
