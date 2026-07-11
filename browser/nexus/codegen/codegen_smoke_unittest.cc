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
// Browser-tool reflection-field protocol: codegen auto-injects three
// required string parameters (`evaluation_previous_goal`, `memory`,
// `next_goal`) on every tool under `declarations/browser_tool/` —
// they're prepended to `schema.parameters[]` and to the typed input
// struct ahead of whatever the .tool.json declared. See
// `_inject_reflection_fields` in `codegen/generate.py`. All four
// representative tools tested here (screenshot / navigate / click /
// done) live under browser_tool/, so every Extract() call below seeds
// the three reflection-field strings.
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

// Number of reflection-field args codegen prepends to every browser-
// tool schema and input struct.
constexpr size_t kReflectionFieldCount = 3;

// Seed every Extract() call's args dict with the three reflection
// fields that codegen treats as required. Without these, every
// browser-tool Extract() returns a `missing required string` error
// before it ever looks at the .tool.json-declared fields.
void SeedReflectionFields(base::DictValue& args) {
  args.Set("evaluation_previous_goal", "No prior goal yet.");
  args.Set("memory", "Smoke-test run; no prior context.");
  args.Set("next_goal", "Run this codegen smoke test to green.");
}

// Same, for assertions on the populated input struct.
template <typename InputT>
void ExpectReflectionFieldsPopulated(const InputT& input) {
  EXPECT_EQ(input.evaluation_previous_goal, "No prior goal yet.");
  EXPECT_EQ(input.memory, "Smoke-test run; no prior context.");
  EXPECT_EQ(input.next_goal, "Run this codegen smoke test to green.");
}

// =============================================================================
// Schema-builder shape tests. Pick four representative declarations:
//   - screenshot: zero declared inputs (reflection fields only)
//   - navigate:   1 required string + 1 optional boolean
//   - click:      1 required integer (no optionals)
//   - done:       2 required mixed-type fields (boolean + string)
// `schema.name` reads from the .tool.json's `wire_name` if set,
// otherwise `name`; all four browser tools tested here have explicit
// `wire_name` entries that we pin below.
// =============================================================================

TEST(CodegenSmokeTest, ScreenshotSchemaShape) {
  ::nexus::ToolSchema schema = BuildScreenshotSchema();
  EXPECT_EQ(schema.name, "BrowserScreenshot");
  EXPECT_FALSE(schema.description.empty());
  EXPECT_EQ(schema.output_kind, ::nexus::JsonSchemaKind::kObject);
  // No declared inputs → only the 3 injected reflection fields remain.
  ASSERT_EQ(schema.parameters.size(), kReflectionFieldCount);
  EXPECT_EQ(schema.parameters[0].name, "evaluation_previous_goal");
  EXPECT_EQ(schema.parameters[1].name, "memory");
  EXPECT_EQ(schema.parameters[2].name, "next_goal");
  for (const auto& p : schema.parameters) {
    EXPECT_EQ(p.kind, ::nexus::JsonSchemaKind::kString);
    EXPECT_TRUE(p.required);
  }
  // Anthropic input_examples block emitted from the .tool.json.
  ASSERT_TRUE(schema.input_examples.has_value());
  EXPECT_FALSE(schema.input_examples->empty());
}

TEST(CodegenSmokeTest, NavigateSchemaShape) {
  ::nexus::ToolSchema schema = BuildNavigateSchema();
  EXPECT_EQ(schema.name, "BrowserNavigate");
  EXPECT_FALSE(schema.description.empty());
  // 3 reflection + url + new_tab = 5.
  ASSERT_EQ(schema.parameters.size(), kReflectionFieldCount + 2);
  // Reflection fields are prepended first.
  EXPECT_EQ(schema.parameters[0].name, "evaluation_previous_goal");
  EXPECT_EQ(schema.parameters[1].name, "memory");
  EXPECT_EQ(schema.parameters[2].name, "next_goal");
  // Declared inputs follow in .tool.json order.
  EXPECT_EQ(schema.parameters[3].name, "url");
  EXPECT_EQ(schema.parameters[3].kind, ::nexus::JsonSchemaKind::kString);
  EXPECT_TRUE(schema.parameters[3].required);
  EXPECT_EQ(schema.parameters[4].name, "new_tab");
  EXPECT_EQ(schema.parameters[4].kind, ::nexus::JsonSchemaKind::kBoolean);
  EXPECT_FALSE(schema.parameters[4].required);
}

TEST(CodegenSmokeTest, ClickSchemaShape) {
  ::nexus::ToolSchema schema = BuildClickSchema();
  EXPECT_EQ(schema.name, "BrowserClick");
  // 3 reflection + index = 4.
  ASSERT_EQ(schema.parameters.size(), kReflectionFieldCount + 1);
  EXPECT_EQ(schema.parameters[0].name, "evaluation_previous_goal");
  EXPECT_EQ(schema.parameters[1].name, "memory");
  EXPECT_EQ(schema.parameters[2].name, "next_goal");
  EXPECT_EQ(schema.parameters[3].name, "index");
  EXPECT_TRUE(schema.parameters[3].required);
  EXPECT_EQ(schema.parameters[3].kind, ::nexus::JsonSchemaKind::kInteger);
}

TEST(CodegenSmokeTest, DoneSchemaShape) {
  ::nexus::ToolSchema schema = BuildDoneSchema();
  EXPECT_EQ(schema.name, "Done");
  // 3 reflection + success + text = 5.
  ASSERT_EQ(schema.parameters.size(), kReflectionFieldCount + 2);
  EXPECT_EQ(schema.parameters[0].name, "evaluation_previous_goal");
  EXPECT_EQ(schema.parameters[1].name, "memory");
  EXPECT_EQ(schema.parameters[2].name, "next_goal");
  // Both declared inputs are required: success (bool) + text (string).
  EXPECT_EQ(schema.parameters[3].name, "success");
  EXPECT_EQ(schema.parameters[3].kind, ::nexus::JsonSchemaKind::kBoolean);
  EXPECT_TRUE(schema.parameters[3].required);
  EXPECT_EQ(schema.parameters[4].name, "text");
  EXPECT_EQ(schema.parameters[4].kind, ::nexus::JsonSchemaKind::kString);
  EXPECT_TRUE(schema.parameters[4].required);
}

// =============================================================================
// Extract() tests: success paths + missing-required error paths.
// Every browser-tool Extract() expects the 3 reflection fields, so each
// success-path test calls SeedReflectionFields() first; the missing-
// required tests OMIT one specific field at a time and assert the
// error names that field.
// =============================================================================

TEST(CodegenSmokeTest, ScreenshotExtractWithReflectionFields) {
  base::DictValue args;
  SeedReflectionFields(args);
  ScreenshotInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  ExpectReflectionFieldsPopulated(input);
}

TEST(CodegenSmokeTest, ScreenshotExtractMissingReflectionField) {
  // Empty dict — every reflection field is missing. The extractor
  // walks them in declared order, so the FIRST one reported is
  // `evaluation_previous_goal`.
  base::DictValue args;
  ScreenshotInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("evaluation_previous_goal"), std::string::npos)
      << "missing reflection-field error should name "
         "evaluation_previous_goal; got: "
      << *err;
}

TEST(CodegenSmokeTest, NavigateExtractRequiredFieldPresent) {
  base::DictValue args;
  SeedReflectionFields(args);
  args.Set("url", "https://example.com");
  NavigateInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.url, "https://example.com");
  EXPECT_FALSE(input.new_tab.has_value());
  ExpectReflectionFieldsPopulated(input);
}

TEST(CodegenSmokeTest, NavigateExtractWithOptional) {
  base::DictValue args;
  SeedReflectionFields(args);
  args.Set("url", "https://example.com");
  args.Set("new_tab", true);
  NavigateInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.url, "https://example.com");
  ASSERT_TRUE(input.new_tab.has_value());
  EXPECT_TRUE(*input.new_tab);
}

TEST(CodegenSmokeTest, NavigateExtractMissingRequiredUrl) {
  // Reflection fields present; url missing. The extractor walks the
  // reflection fields first, then the declared inputs; the first
  // failing field stops extraction and names itself in the error.
  base::DictValue args;
  SeedReflectionFields(args);
  NavigateInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("url"), std::string::npos)
      << "missing-required error should name `url`; got: " << *err;
}

TEST(CodegenSmokeTest, DoneExtractBothRequiredPresent) {
  base::DictValue args;
  SeedReflectionFields(args);
  args.Set("success", true);
  args.Set("text", "Found it.");
  DoneInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_TRUE(input.success);
  EXPECT_EQ(input.text, "Found it.");
}

TEST(CodegenSmokeTest, DoneExtractMissingText) {
  base::DictValue args;
  SeedReflectionFields(args);
  args.Set("success", true);
  // No text → required-string error after the reflection fields and
  // success pass.
  DoneInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("text"), std::string::npos)
      << "missing 'text' should be named in the error; got: " << *err;
}

TEST(CodegenSmokeTest, ClickExtractRequiredIndexPresent) {
  base::DictValue args;
  SeedReflectionFields(args);
  args.Set("index", 12);
  ClickInput input;
  EXPECT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.index, 12);
}

TEST(CodegenSmokeTest, ClickExtractMissingIndex) {
  base::DictValue args;
  SeedReflectionFields(args);
  // No index → required-integer error.
  ClickInput input;
  std::optional<std::string> err = Extract(args, input);
  ASSERT_TRUE(err.has_value());
  EXPECT_NE(err->find("index"), std::string::npos);
}

}  // namespace
}  // namespace nexus::generated
