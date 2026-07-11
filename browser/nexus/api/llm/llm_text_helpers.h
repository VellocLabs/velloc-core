// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_TEXT_HELPERS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_TEXT_HELPERS_H_

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"

namespace nexus {

// JSON serialize helper used when flattening non-string Value nodes into the
// system-instruction text. Returns the JSON form of |value| (never throws).
std::string SerializeValueToJson(const base::Value& value);

// JSON parse helper. Returns std::nullopt if |json| is not valid RFC JSON.
std::optional<base::Value> ParseJsonValue(const std::string& json);

// Recursively flattens a Value into a list of text parts. Strings become
// their own entries; Lists recurse; non-string/non-list Values are
// JSON-serialized so they appear verbatim in the flattened stream.
// Empty strings are skipped (matches the prior inline behavior in
// nexus_llm_host.cc).
void AppendFlattenedText(const base::Value& value,
                         std::vector<std::string>* out);

// Convenience wrapper: returns AppendFlattenedText joined with "\n". Empty
// when |value| is null or flattens to nothing.
std::string FlattenTextValue(const base::Value* value);

// Renders a DictValue as a "- key: value" markdown-ish section under a
// heading |title|. Each entry's value is flattened via FlattenTextValue,
// so nested structures are inlined cleanly. Returns an empty string when
// |context| is null or empty.
std::string FormatContextSection(const char* title,
                                 const base::DictValue* context);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_TEXT_HELPERS_H_
