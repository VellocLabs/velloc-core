// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool.h"

#include <string>
#include <vector>

#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(BrowserToolSchemaTest, ExposesOnlyBrowserActionsTool) {
  ToolRegistry registry;
  BrowserTool::DefineTools(registry);

  std::vector<ToolSchema> schemas = registry.CollectSchemas();
  ASSERT_EQ(schemas.size(), 1u);
  EXPECT_EQ(schemas[0].name, "browser_actions");
  EXPECT_EQ(schemas[0].parameters.size(), 4u);

  EXPECT_EQ(schemas[0].parameters[0].name, "evaluation_previous_goal");
  EXPECT_EQ(schemas[0].parameters[1].name, "memory");
  EXPECT_EQ(schemas[0].parameters[2].name, "next_goal");
  EXPECT_EQ(schemas[0].parameters[3].name, "actions");
  EXPECT_EQ(schemas[0].parameters[3].kind, JsonSchemaKind::kArrayOfObject);

  // Reflection fields (browser-use mechanism #1 — model self-report):
  // schema-required so the JSON Schema's `required[]` array gives the
  // LLM a strong training-time signal to fill all three every turn,
  // anchoring the multi-step planning loop. Extraction stays lenient
  // (the C++ method takes std::optional<std::string>), so a model
  // that drops one still arrives as nullopt — no `invalid_args`
  // rejection, no `args:{}` spiral risk. See comment block in
  // browser_tool.cc on the actions field for the rationale on the
  // schema-required-plus-optional-extraction combo.
  EXPECT_TRUE(schemas[0].parameters[0].required);
  EXPECT_TRUE(schemas[0].parameters[1].required);
  EXPECT_TRUE(schemas[0].parameters[2].required);
  EXPECT_TRUE(schemas[0].parameters[3].required);

  for (const ToolSchema& schema : schemas) {
    EXPECT_NE(schema.name, "browser_click");
    EXPECT_NE(schema.name, "browser_type");
    EXPECT_NE(schema.name, "browser_navigate");
    EXPECT_NE(schema.name, "browser_search");
  }
}

TEST(BrowserToolSchemaTest, DescriptionDocumentsNewActionNames) {
  ToolRegistry registry;
  BrowserTool::DefineTools(registry);

  std::vector<ToolSchema> schemas = registry.CollectSchemas();
  ASSERT_EQ(schemas.size(), 1u);
  const std::string& description = schemas[0].description;

  EXPECT_NE(description.find("click"), std::string::npos);
  EXPECT_NE(description.find("input"), std::string::npos);
  EXPECT_NE(description.find("navigate"), std::string::npos);
  EXPECT_NE(description.find("read_page"), std::string::npos);
  EXPECT_EQ(description.find("browser_click"), std::string::npos);
  EXPECT_EQ(description.find("browser_type"), std::string::npos);
  EXPECT_EQ(description.find("node_id"), std::string::npos);
}

}  // namespace
}  // namespace nexus
