// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Codegen-smoke: fast tests that verify the per-tool inline
// `Build<Name>Schema()` and `Extract()` functions emitted by
// generate.py compile and produce the right shape.
//
// Lives in `unit_tests_lite` (not `unit_tests`) so it runs on every
// `scripts/run_nexus_tests.sh` invocation without pulling in the full
// bridge (which transitively drags in //chrome/browser, content::,
// etc.). That means we DON'T test `PopulateRegistry()` here — that
// function references `CategoryAccessor<BrowserTool>::Get(session)`
// which lives in browser_tool.cc and pulls in the heavy stack.
// `PopulateRegistry()` IS tested end-to-end by the bridge build
// itself (any registration breakage causes a link error) — and a
// follow-up unit test in the heavy `unit_tests` target can cover the
// dispatcher trampoline routing once we want belt-and-braces.
//
// What this file tests:
//   (1) Each tool's Build<Name>Schema() returns a fully populated
//       ToolSchema (name, description, parameter count, required-ness).
//   (2) Each tool's Extract() correctly populates the typed input
//       struct from a base::DictValue, including the missing-required
//       error path.
//
// Adding a new declaration: drop a corresponding test block. The
// codegen guarantees the include below still resolves and the
// schema-builder symbol still exists, so test failures here mean
// either (a) the codegen regressed, or (b) the .tool.json's expected
// shape diverged from what this file asserts.

#include "base/values.h"
#include "custom_browser/browser/nexus/tools/click.gen.h"
#include "custom_browser/browser/nexus/tools/done.gen.h"
#include "custom_browser/browser/nexus/tools/navigate.gen.h"
#include "custom_browser/browser/nexus/tools/screenshot.gen.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus::generated {
namespace {

// =============================================================================
// Schema-builder shape tests. Pick four representative declarations:
//   - screenshot: optional-only inputs (no required fields)
//   - navigate:   1 required string + 1 optional integer
//   - click:      1 required integer + 3 optionals (mixed)
//   - done:       2 required mixed-type fields (boolean + string)
// =============================================================================

TEST(CodegenSmokeTest, ScreenshotSchemaShape) {
  ::nexus::ToolSchema schema = BuildScreenshotSchema();
  EXPECT_EQ(schema.name, "screenshot");
  EXPECT_FALSE(schema.description.empty());
  EXPECT_EQ(schema.output_kind, ::nexus::JsonSchemaKind::kObject);
  ASSERT_EQ(schema.parameters.size(), 1u);
  EXPECT_EQ(schema.parameters[0].name, "tab_id");
  EXPECT_EQ(schema.parameters[0].kind, ::nexus::JsonSchemaKind::kInteger);
  EXPECT_FALSE(schema.parameters[0].required);
  // Anthropic input_examples block emitted from the .tool.json.
  ASSERT_TRUE(schema.input_examples.has_value());
  EXPECT_FALSE(schema.input_examples->empty());
}

TEST(CodegenSmokeTest, NavigateSchemaShape) {
  ::nexus::ToolSchema schema = BuildNavigateSchema();
  EXPECT_EQ(schema.name, "navigate");
  EXPECT_FALSE(schema.description.empty());
  ASSERT_EQ(schema.parameters.size(), 2u);
  // url is required and listed first per the .tool.json's input order.
  EXPECT_EQ(schema.parameters[0].name, "url");
  EXPECT_EQ(schema.parameters[0].kind, ::nexus::JsonSchemaKind::kString);
  EXPECT_TRUE(schema.parameters[0].required);
  // tab_id is the optional follow-up.
  EXPECT_EQ(schema.parameters[1].name, "tab_id");
  EXPECT_EQ(schema.parameters[1].kind, ::nexus::JsonSchemaKind::kInteger);
  EXPECT_FALSE(schema.parameters[1].required);
}

TEST(CodegenSmokeTest, ClickSchemaShape) {
  ::nexus::ToolSchema schema = BuildClickSchema();
  EXPECT_EQ(schema.name, "click");
  ASSERT_EQ(schema.parameters.size(), 4u);
  EXPECT_EQ(schema.parameters[0].name, "index");
  EXPECT_TRUE(schema.parameters[0].required);
  EXPECT_EQ(schema.parameters[0].kind, ::nexus::JsonSchemaKind::kInteger);
  // Remaining three are optional: tab_id, button, click_count.
  EXPECT_FALSE(schema.parameters[1].required);
  EXPECT_FALSE(schema.parameters[2].required);
  EXPECT_FALSE(schema.parameters[3].required);
}

TEST(CodegenSmokeTest, DoneSchemaShape) {
  ::nexus::ToolSchema schema = BuildDoneSchema();
  EXPECT_EQ(schema.name, "done");
  ASSERT_EQ(schema.parameters.size(), 2u);
  // Both required: success (bool) + text (string).
  EXPECT_TRUE(schema.parameters[0].required);
  EXPECT_TRUE(schema.parameters[1].required);
  // The two have different kinds; we don't pin order beyond what
  // matches the .tool.json's inputs[] ordering.
  EXPECT_EQ(schema.parameters[0].name, "success");
  EXPECT_EQ(schema.parameters[0].kind, ::nexus::JsonSchemaKind::kBoolean);
  EXPECT_EQ(schema.parameters[1].name, "text");
  EXPECT_EQ(schema.parameters[1].kind, ::nexus::JsonSchemaKind::kString);
}

// =============================================================================
// Extract() tests: success paths + missing-required error paths.
// =============================================================================

TEST(CodegenSmokeTest, ScreenshotExtractEmptyDict) {
  // No required fields → empty dict is valid.
  base::DictValue args;
  ScreenshotInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_FALSE(input.tab_id.has_value());
}

TEST(CodegenSmokeTest, ScreenshotExtractWithOptional) {
  base::DictValue args;
  args.Set("tab_id", 42);
  ScreenshotInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  ASSERT_TRUE(input.tab_id.has_value());
  EXPECT_EQ(*input.tab_id, 42);
}

TEST(CodegenSmokeTest, NavigateExtractRequiredFieldPresent) {
  base::DictValue args;
  args.Set("url", "https://example.com");
  NavigateInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.url, "https://example.com");
  EXPECT_FALSE(input.tab_id.has_value());
}

TEST(CodegenSmokeTest, NavigateExtractWithOptional) {
  base::DictValue args;
  args.Set("url", "https://example.com");
  args.Set("tab_id", 7);
  NavigateInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.url, "https://example.com");
  ASSERT_TRUE(input.tab_id.has_value());
  EXPECT_EQ(*input.tab_id, 7);
}

TEST(CodegenSmokeTest, NavigateExtractMissingRequired) {
  // Empty dict is now a SHAPE error because url is required. This is
  // the structural fix the split-tool migration delivers: the
  // extractor rejects empty-args before the body sees the call.
  base::DictValue args;
  NavigateInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("url"), std::string::npos)
      << "missing-required error should name the missing field; got: "
      << *err;
}

TEST(CodegenSmokeTest, DoneExtractBothRequiredPresent) {
  base::DictValue args;
  args.Set("success", true);
  args.Set("text", "Found it.");
  DoneInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_TRUE(input.success);
  EXPECT_EQ(input.text, "Found it.");
}

TEST(CodegenSmokeTest, DoneExtractMissingText) {
  base::DictValue args;
  args.Set("success", true);
  // No text → required-string error.
  DoneInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("text"), std::string::npos)
      << "missing 'text' should be named in the error; got: " << *err;
}

TEST(CodegenSmokeTest, ClickExtractRequiredIndexPresent) {
  base::DictValue args;
  args.Set("index", 12);
  ClickInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.index, 12);
  EXPECT_FALSE(input.tab_id.has_value());
  EXPECT_FALSE(input.button.has_value());
  EXPECT_FALSE(input.click_count.has_value());
}

TEST(CodegenSmokeTest, ClickExtractMissingIndex) {
  base::DictValue args;
  // No index → required-integer error.
  ClickInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("index"), std::string::npos);
}

}  // namespace
}  // namespace nexus::generated
