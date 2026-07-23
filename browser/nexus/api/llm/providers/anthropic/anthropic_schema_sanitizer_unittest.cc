// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_schema_sanitizer.h"

#include <optional>

#include "base/json/json_reader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue Parse(const char* json) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  CHECK(parsed.has_value() && parsed->is_dict());
  return std::move(*parsed).TakeDict();
}

// The authored OpenAI-strict dialect: every property required, optionals
// expressed as nullable-required. The sanitizer must produce claude_code's
// plain-optional form (zod output): null members gone, nullable fields out
// of `required`, everything else preserved.
TEST(AnthropicSchemaSanitizerTest, CollapsesNullableTypesAndRewritesRequired) {
  base::DictValue schema = Parse(R"({
    "type": "object",
    "properties": {
      "pattern": {"type": "string", "description": "regex"},
      "path": {"type": ["string", "null"]},
      "head_limit": {"type": ["integer", "null"]},
      "multiline": {"type": ["boolean", "null"]}
    },
    "required": ["pattern", "path", "head_limit", "multiline"],
    "additionalProperties": false
  })");

  base::DictValue out = SanitizeStrictSchemaForAnthropic(schema);

  // Nullable unions collapse to their base type.
  EXPECT_EQ(*out.FindStringByDottedPath("properties.path.type"), "string");
  EXPECT_EQ(*out.FindStringByDottedPath("properties.head_limit.type"),
            "integer");
  EXPECT_EQ(*out.FindStringByDottedPath("properties.multiline.type"),
            "boolean");
  // The non-nullable property is untouched.
  EXPECT_EQ(*out.FindStringByDottedPath("properties.pattern.type"), "string");
  EXPECT_EQ(*out.FindStringByDottedPath("properties.pattern.description"),
            "regex");

  // Only the genuinely-required property stays in `required`.
  const base::ListValue* required = out.FindList("required");
  ASSERT_TRUE(required);
  ASSERT_EQ(required->size(), 1u);
  EXPECT_EQ((*required)[0].GetString(), "pattern");

  // additionalProperties:false is preserved (Anthropic strict requires it).
  EXPECT_EQ(out.FindBool("additionalProperties"), false);
  // The full property surface is preserved — nothing deleted.
  EXPECT_EQ(out.FindDict("properties")->size(), 4u);
}

TEST(AnthropicSchemaSanitizerTest, StripsNullFromEnums) {
  base::DictValue schema = Parse(R"({
    "type": "object",
    "properties": {
      "output_mode": {
        "type": ["string", "null"],
        "enum": [null, "files_with_matches", "content", "count"]
      }
    },
    "required": ["output_mode"],
    "additionalProperties": false
  })");

  base::DictValue out = SanitizeStrictSchemaForAnthropic(schema);

  EXPECT_EQ(*out.FindStringByDottedPath("properties.output_mode.type"),
            "string");
  const base::ListValue* enum_list =
      out.FindListByDottedPath("properties.output_mode.enum");
  ASSERT_TRUE(enum_list);
  ASSERT_EQ(enum_list->size(), 3u);
  EXPECT_EQ((*enum_list)[0].GetString(), "files_with_matches");
  EXPECT_EQ((*enum_list)[1].GetString(), "content");
  EXPECT_EQ((*enum_list)[2].GetString(), "count");
  // The enum-nullable property drops out of `required`.
  const base::ListValue* required = out.FindList("required");
  ASSERT_TRUE(required);
  EXPECT_TRUE(required->empty());
}

TEST(AnthropicSchemaSanitizerTest, RecursesNestedObjectsItemsAndCombinators) {
  base::DictValue schema = Parse(R"({
    "type": "object",
    "properties": {
      "todos": {
        "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "content": {"type": "string"},
            "note": {"type": ["string", "null"]}
          },
          "required": ["content", "note"],
          "additionalProperties": false
        }
      },
      "mode": {
        "anyOf": [
          {"type": "string"},
          {"type": "null"}
        ]
      }
    },
    "required": ["todos", "mode"],
    "additionalProperties": false
  })");

  base::DictValue out = SanitizeStrictSchemaForAnthropic(schema);

  // Nested item object: nullable member collapsed + its required rewritten.
  EXPECT_EQ(*out.FindStringByDottedPath("properties.todos.items.properties.note.type"),
            "string");
  const base::ListValue* item_required =
      out.FindListByDottedPath("properties.todos.items.required");
  ASSERT_TRUE(item_required);
  ASSERT_EQ(item_required->size(), 1u);
  EXPECT_EQ((*item_required)[0].GetString(), "content");

  // anyOf: the {"type":"null"} branch is dropped and marks the property
  // optional at the parent.
  const base::ListValue* any_of =
      out.FindListByDottedPath("properties.mode.anyOf");
  ASSERT_TRUE(any_of);
  ASSERT_EQ(any_of->size(), 1u);
  const base::ListValue* required = out.FindList("required");
  ASSERT_TRUE(required);
  ASSERT_EQ(required->size(), 1u);
  EXPECT_EQ((*required)[0].GetString(), "todos");
}

TEST(AnthropicSchemaSanitizerTest, StripsNumericRangeKeywords) {
  // Regression: claude_code's zod reference emits numeric bounds (Read:
  // offset >= 0, limit >= 1) and they landed in read.tool.json's
  // parametersStrict. OpenAI's structured-outputs dialect accepts them, but
  // Anthropic's strict tool schema rejects `minimum` on an integer with a
  // 400 (`tools.N.custom: For 'integer' type, property 'minimum' is not
  // supported`). The sanitizer must drop every numeric-range keyword while
  // still collapsing the nullable type and preserving the rest of the node.
  base::DictValue schema = Parse(R"({
    "type": "object",
    "properties": {
      "file_path": {"type": "string", "description": "path"},
      "offset": {"type": ["integer", "null"], "minimum": 0},
      "limit": {"type": ["integer", "null"], "minimum": 1},
      "max_results": {
        "type": ["integer", "null"],
        "maximum": 100,
        "exclusiveMinimum": 0,
        "exclusiveMaximum": 101,
        "multipleOf": 2
      }
    },
    "required": ["file_path", "offset", "limit", "max_results"],
    "additionalProperties": false
  })");

  base::DictValue out = SanitizeStrictSchemaForAnthropic(schema);

  // Nullable integer types still collapse to their base type...
  EXPECT_EQ(*out.FindStringByDottedPath("properties.offset.type"), "integer");
  EXPECT_EQ(*out.FindStringByDottedPath("properties.limit.type"), "integer");
  EXPECT_EQ(*out.FindStringByDottedPath("properties.max_results.type"),
            "integer");

  // ...and every numeric-range keyword is gone.
  EXPECT_EQ(out.FindByDottedPath("properties.offset.minimum"), nullptr);
  EXPECT_EQ(out.FindByDottedPath("properties.limit.minimum"), nullptr);
  EXPECT_EQ(out.FindByDottedPath("properties.max_results.maximum"), nullptr);
  EXPECT_EQ(out.FindByDottedPath("properties.max_results.exclusiveMinimum"),
            nullptr);
  EXPECT_EQ(out.FindByDottedPath("properties.max_results.exclusiveMaximum"),
            nullptr);
  EXPECT_EQ(out.FindByDottedPath("properties.max_results.multipleOf"),
            nullptr);

  // Sibling keys on the same node survive.
  EXPECT_EQ(*out.FindStringByDottedPath("properties.file_path.description"),
            "path");
  // The nullable range-constrained fields drop out of `required`.
  const base::ListValue* required = out.FindList("required");
  ASSERT_TRUE(required);
  ASSERT_EQ(required->size(), 1u);
  EXPECT_EQ((*required)[0].GetString(), "file_path");
}

TEST(AnthropicSchemaSanitizerTest, PlainOptionalSchemaPassesThroughUnchanged) {
  // A permissive (non-strict-dialect) schema has nothing to sanitize.
  base::DictValue schema = Parse(R"({
    "type": "object",
    "properties": {
      "command": {"type": "string"},
      "timeout": {"type": "integer"}
    },
    "required": ["command"],
    "additionalProperties": false
  })");

  base::DictValue out = SanitizeStrictSchemaForAnthropic(schema);
  EXPECT_EQ(out, schema);
}

}  // namespace
}  // namespace nexus
