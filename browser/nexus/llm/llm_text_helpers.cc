// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/llm/llm_text_helpers.h"

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"

namespace nexus {

std::string SerializeValueToJson(const base::Value& value) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  return json;
}

std::optional<base::Value> ParseJsonValue(const std::string& json) {
  return base::JSONReader::Read(json, base::JSON_PARSE_RFC);
}

void AppendFlattenedText(const base::Value& value,
                         std::vector<std::string>* out) {
  if (const std::string* text = value.GetIfString(); text && !text->empty()) {
    out->push_back(*text);
    return;
  }
  if (const base::ListValue* list = value.GetIfList()) {
    for (const auto& entry : *list) {
      AppendFlattenedText(entry, out);
    }
    return;
  }
  if (!value.is_none()) {
    out->push_back(SerializeValueToJson(value));
  }
}

std::string FlattenTextValue(const base::Value* value) {
  if (!value) {
    return std::string();
  }
  std::vector<std::string> parts;
  AppendFlattenedText(*value, &parts);
  std::string result;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!result.empty()) {
      result.append("\n");
    }
    result.append(part);
  }
  return result;
}

std::string FormatContextSection(const char* title,
                                 const base::DictValue* context) {
  if (!context || context->empty()) {
    return std::string();
  }
  std::string section(title);
  section.append(":\n");
  bool first = true;
  for (const auto [key, value] : *context) {
    if (!first) {
      section.append("\n");
    }
    first = false;
    section.append("- ");
    section.append(key);
    section.append(": ");
    section.append(FlattenTextValue(&value));
  }
  return section;
}

}  // namespace nexus
