// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_schema_sanitizer.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace nexus {

namespace {

// Collapses a nullable type union in place. Returns true when the value WAS
// nullable (a "null" member was removed) so the caller can drop the property
// from `required`.
bool CollapseNullableType(base::DictValue& schema) {
  base::ListValue* type_list = schema.FindList("type");
  if (!type_list) {
    return false;
  }
  base::ListValue remaining;
  bool had_null = false;
  for (const auto& entry : *type_list) {
    if (entry.is_string() && entry.GetString() == "null") {
      had_null = true;
      continue;
    }
    remaining.Append(entry.Clone());
  }
  if (!had_null) {
    return false;
  }
  if (remaining.size() == 1u && remaining.front().is_string()) {
    // ["T", "null"] → "T" — the common authored form.
    schema.Set("type", remaining.front().Clone());
  } else {
    // A residual multi-type union stays a (null-free) array.
    schema.Set("type", std::move(remaining));
  }
  return true;
}

// Strips a literal null member from an `enum` array in place. Returns true
// when a null was removed (the enum expressed optionality).
bool StripNullFromEnum(base::DictValue& schema) {
  base::ListValue* enum_list = schema.FindList("enum");
  if (!enum_list) {
    return false;
  }
  base::ListValue remaining;
  bool had_null = false;
  for (const auto& entry : *enum_list) {
    if (entry.is_none()) {
      had_null = true;
      continue;
    }
    remaining.Append(entry.Clone());
  }
  if (!had_null) {
    return false;
  }
  schema.Set("enum", std::move(remaining));
  return true;
}

// Recursively sanitizes `schema`. Returns true when THIS node was nullable
// (so an enclosing object should drop it from `required`).
bool SanitizeNode(base::DictValue& schema) {
  bool nullable = CollapseNullableType(schema);
  nullable |= StripNullFromEnum(schema);

  // Numeric-range constraints. The strict schema is authored in OpenAI's
  // structured-outputs dialect, which accepts these (and claude_code's zod
  // reference emits them — e.g. Read's `offset >= 0`, `limit >= 1`).
  // Anthropic's strict tool schema rejects them with a 400
  // (`For 'integer' type, property 'minimum' is not supported`), so strip
  // them wherever they appear. They only tighten grammar-constrained
  // sampling; dropping them changes no structure and leaves the constraint
  // intact in the OpenAI-dialect schema other providers consume.
  for (const char* range_key : {"minimum", "maximum", "exclusiveMinimum",
                                "exclusiveMaximum", "multipleOf"}) {
    schema.Remove(range_key);
  }

  // Object properties: sanitize each, collecting which became optional, then
  // rewrite this object's `required` without them.
  if (base::DictValue* properties = schema.FindDict("properties")) {
    std::vector<std::string> now_optional;
    for (auto entry : *properties) {
      base::DictValue* prop = entry.second.GetIfDict();
      if (prop && SanitizeNode(*prop)) {
        now_optional.push_back(entry.first);
      }
    }
    if (!now_optional.empty()) {
      if (base::ListValue* required = schema.FindList("required")) {
        base::ListValue kept;
        for (const auto& name : *required) {
          if (name.is_string() &&
              std::find(now_optional.begin(), now_optional.end(),
                        name.GetString()) != now_optional.end()) {
            continue;
          }
          kept.Append(name.Clone());
        }
        schema.Set("required", std::move(kept));
      }
    }
  }

  // Array items.
  if (base::DictValue* items = schema.FindDict("items")) {
    SanitizeNode(*items);
  }

  // Local definitions.
  for (const char* defs_key : {"$defs", "definitions"}) {
    if (base::DictValue* defs = schema.FindDict(defs_key)) {
      for (auto entry : *defs) {
        if (base::DictValue* def = entry.second.GetIfDict()) {
          SanitizeNode(*def);
        }
      }
    }
  }

  // Combinators. A `{"type": "null"}` alternative inside anyOf/oneOf also
  // expresses optionality — drop it and surface nullability to the parent.
  for (const char* combinator : {"anyOf", "oneOf", "allOf"}) {
    base::ListValue* branches = schema.FindList(combinator);
    if (!branches) {
      continue;
    }
    base::ListValue kept;
    for (auto& branch : *branches) {
      base::DictValue* branch_dict = branch.GetIfDict();
      if (branch_dict) {
        const std::string* branch_type = branch_dict->FindString("type");
        if (branch_type && *branch_type == "null") {
          nullable = true;
          continue;
        }
        SanitizeNode(*branch_dict);
      }
      kept.Append(std::move(branch));
    }
    schema.Set(combinator, std::move(kept));
  }

  return nullable;
}

}  // namespace

base::DictValue SanitizeStrictSchemaForAnthropic(
    const base::DictValue& schema) {
  base::DictValue sanitized = schema.Clone();
  SanitizeNode(sanitized);
  return sanitized;
}

}  // namespace nexus
