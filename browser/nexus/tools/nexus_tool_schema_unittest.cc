// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

ToolParameter MakeParam(const std::string& name,
                         const std::string& description,
                         JsonSchemaKind kind,
                         bool required) {
  ToolParameter p;
  p.name = name;
  p.description = description;
  p.kind = kind;
  p.required = required;
  return p;
}

TEST(JsonSchemaTypeStringTest, ScalarKindsMapToTheirJsonNames) {
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kString), "string");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kInteger), "integer");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kNumber), "number");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kBoolean), "boolean");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kObject), "object");
}

TEST(JsonSchemaTypeStringTest, ArrayKindsAllMapToArray) {
  // Array element type is encoded separately via the `items` clause —
  // see BuildHostedToolDefinitionDict tests below.
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kArrayOfString), "array");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kArrayOfInteger), "array");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kArrayOfNumber), "array");
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kArrayOfObject), "array");
}

TEST(JsonSchemaTypeStringTest, AnyMapsToObjectAsTheMostPermissiveType) {
  EXPECT_STREQ(JsonSchemaTypeString(JsonSchemaKind::kAny), "object");
}

TEST(BuildHostedToolDefinitionDictTest, BasicShapeMatchesWireFormat) {
  ToolSchema schema;
  schema.name = "example_navigate";
  schema.description = "Navigate the tab to a URL.";
  schema.parameters.push_back(
      MakeParam("tab_id", "Optional. Active tab if absent.",
                JsonSchemaKind::kInteger, /*required=*/false));
  schema.parameters.push_back(
      MakeParam("url", "Target URL.", JsonSchemaKind::kString,
                /*required=*/true));

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);
  ASSERT_NE(dict.FindString("type"), nullptr);
  EXPECT_EQ(*dict.FindString("type"), "function");
  EXPECT_EQ(*dict.FindString("name"), "example_navigate");
  EXPECT_EQ(*dict.FindString("description"), "Navigate the tab to a URL.");

  const base::DictValue* params = dict.FindDict("parameters");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(*params->FindString("type"), "object");

  const base::DictValue* properties = params->FindDict("properties");
  ASSERT_NE(properties, nullptr);
  // tab_id (optional, integer)
  const base::DictValue* tab_id = properties->FindDict("tab_id");
  ASSERT_NE(tab_id, nullptr);
  EXPECT_EQ(*tab_id->FindString("type"), "integer");
  EXPECT_EQ(*tab_id->FindString("description"),
            "Optional. Active tab if absent.");
  // url (required, string)
  const base::DictValue* url = properties->FindDict("url");
  ASSERT_NE(url, nullptr);
  EXPECT_EQ(*url->FindString("type"), "string");
  EXPECT_EQ(*url->FindString("description"), "Target URL.");

  // required[] should list "url" only.
  const base::ListValue* required = params->FindList("required");
  ASSERT_NE(required, nullptr);
  ASSERT_EQ(required->size(), 1u);
  EXPECT_EQ(required->front().GetString(), "url");
}

TEST(BuildHostedToolDefinitionDictTest, EmitsConcurrencySafeWhenMarked) {
  ToolSchema schema;
  schema.name = "Read";
  schema.description = "Read a file.";
  schema.concurrency_safe = true;

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);

  std::optional<bool> concurrency_safe = dict.FindBool("concurrencySafe");
  ASSERT_TRUE(concurrency_safe.has_value());
  EXPECT_TRUE(*concurrency_safe);
}

TEST(BuildHostedToolDefinitionDictTest, EmitsConcurrencySafeFalseByDefault) {
  ToolSchema schema;
  schema.name = "BrowserReadPage";
  schema.description = "Read the current page.";

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);

  std::optional<bool> concurrency_safe = dict.FindBool("concurrencySafe");
  ASSERT_TRUE(concurrency_safe.has_value());
  EXPECT_FALSE(*concurrency_safe);
}

TEST(BuildHostedToolDefinitionDictTest, OmitsRequiredArrayWhenEmpty) {
  // A schema where every parameter is optional should NOT emit a
  // `required: []` array — JSON Schema treats absent vs empty
  // identically but the wire format we ship to the JS guest expects
  // the field to be absent (matches the JS-side normalizer).
  ToolSchema schema;
  schema.name = "example_back";
  schema.description = "Go back.";
  schema.parameters.push_back(
      MakeParam("tab_id", "Optional. Active tab if absent.",
                JsonSchemaKind::kInteger, /*required=*/false));

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);
  const base::DictValue* params = dict.FindDict("parameters");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(params->FindList("required"), nullptr);
}

TEST(BuildHostedToolDefinitionDictTest, EmptyParameterDescriptionIsOmitted) {
  ToolSchema schema;
  schema.name = "x";
  schema.description = "x";
  schema.parameters.push_back(
      MakeParam("p", /*description=*/"", JsonSchemaKind::kString,
                /*required=*/true));

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);
  const base::DictValue* p = dict.FindDict("parameters")
                                 ->FindDict("properties")
                                 ->FindDict("p");
  ASSERT_NE(p, nullptr);
  // Empty description should NOT emit the field — the JS-side
  // normalizer treats absent/empty identically and we don't want to
  // ship empty strings on the wire.
  EXPECT_EQ(p->FindString("description"), nullptr);
  EXPECT_EQ(*p->FindString("type"), "string");
}

TEST(BuildHostedToolDefinitionDictTest, ArrayParameterEmitsItemsClause) {
  ToolSchema schema;
  schema.name = "x";
  schema.description = "x";
  schema.parameters.push_back(
      MakeParam("names", "List of names",
                JsonSchemaKind::kArrayOfString, /*required=*/true));

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);
  const base::DictValue* names = dict.FindDict("parameters")
                                     ->FindDict("properties")
                                     ->FindDict("names");
  ASSERT_NE(names, nullptr);
  EXPECT_EQ(*names->FindString("type"), "array");
  const base::DictValue* items = names->FindDict("items");
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(*items->FindString("type"), "string");
}

TEST(BuildHostedToolDefinitionDictTest,
     ActionsArrayEmitsBrowserActionConstraints) {
  ToolSchema schema;
  schema.name = "browser_actions";
  schema.description = "Submit browser actions.";
  schema.parameters.push_back(
      MakeParam("actions", "Browser Use-style action batch.",
                JsonSchemaKind::kArrayOfObject, /*required=*/true));

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);
  const base::DictValue* actions = dict.FindDict("parameters")
                                       ->FindDict("properties")
                                       ->FindDict("actions");
  ASSERT_NE(actions, nullptr);
  EXPECT_EQ(*actions->FindString("type"), "array");
  EXPECT_EQ(actions->FindInt("minItems"), 1);
  EXPECT_EQ(actions->FindInt("maxItems"), 5);

  const base::DictValue* items = actions->FindDict("items");
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(*items->FindString("type"), "object");

  const base::DictValue* item_properties = items->FindDict("properties");
  ASSERT_NE(item_properties, nullptr);
  const base::DictValue* type = item_properties->FindDict("type");
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(*type->FindString("type"), "string");
  const std::string* type_description = type->FindString("description");
  ASSERT_NE(type_description, nullptr);
  EXPECT_NE(type_description->find("click"), std::string::npos);
  EXPECT_NE(type_description->find("input"), std::string::npos);

  const base::ListValue* item_required = items->FindList("required");
  ASSERT_NE(item_required, nullptr);
  ASSERT_EQ(item_required->size(), 1u);
  EXPECT_EQ(item_required->front().GetString(), "type");
}

TEST(BuildHostedToolDefinitionDictTest, AllArrayKindsEmitMatchingItemsType) {
  struct Case {
    JsonSchemaKind kind;
    const char* expected_item_type;
  };
  const Case cases[] = {
      {JsonSchemaKind::kArrayOfString, "string"},
      {JsonSchemaKind::kArrayOfInteger, "integer"},
      {JsonSchemaKind::kArrayOfNumber, "number"},
      {JsonSchemaKind::kArrayOfObject, "object"},
  };
  for (const auto& c : cases) {
    ToolSchema schema;
    schema.name = "x";
    schema.description = "x";
    schema.parameters.push_back(
        MakeParam("p", "", c.kind, /*required=*/true));

    base::DictValue dict = BuildHostedToolDefinitionDict(schema);
    const base::DictValue* prop = dict.FindDict("parameters")
                                      ->FindDict("properties")
                                      ->FindDict("p");
    ASSERT_NE(prop, nullptr);
    EXPECT_EQ(*prop->FindString("type"), "array");
    const base::DictValue* items = prop->FindDict("items");
    ASSERT_NE(items, nullptr) << "kind=" << static_cast<int>(c.kind);
    EXPECT_EQ(*items->FindString("type"), c.expected_item_type)
        << "kind=" << static_cast<int>(c.kind);
  }
}

TEST(BuildHostedToolDefinitionDictTest, NoParameterOmitsRequiredAndProperties) {
  ToolSchema schema;
  schema.name = "browser_skill_previews";
  schema.description = "List skills.";

  base::DictValue dict = BuildHostedToolDefinitionDict(schema);
  const base::DictValue* params = dict.FindDict("parameters");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(*params->FindString("type"), "object");
  // properties is always emitted (possibly empty); required is omitted
  // when there are no required params.
  const base::DictValue* properties = params->FindDict("properties");
  ASSERT_NE(properties, nullptr);
  EXPECT_TRUE(properties->empty());
  EXPECT_EQ(params->FindList("required"), nullptr);
}

TEST(BuildHostedToolsListTest, EmitsOneEntryPerSchema) {
  std::vector<ToolSchema> schemas;
  ToolSchema a;
  a.name = "alpha";
  a.description = "first tool";
  ToolSchema b;
  b.name = "beta";
  b.description = "second tool";
  schemas.push_back(a);
  schemas.push_back(b);

  base::ListValue list = BuildHostedToolsList(schemas);
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(*list[0].GetDict().FindString("name"), "alpha");
  EXPECT_EQ(*list[1].GetDict().FindString("name"), "beta");
}

TEST(BuildHostedToolsListTest, EmptySchemaListProducesEmptyList) {
  std::vector<ToolSchema> schemas;
  base::ListValue list = BuildHostedToolsList(schemas);
  EXPECT_TRUE(list.empty());
}

}  // namespace
}  // namespace nexus
