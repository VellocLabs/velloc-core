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

#include <initializer_list>
#include <set>
#include <string>

#include "base/values.h"
#include "custom_browser/browser/nexus/tools/bash.gen.h"
#include "custom_browser/browser/nexus/tools/click.gen.h"
#include "custom_browser/browser/nexus/tools/done.gen.h"
#include "custom_browser/browser/nexus/tools/edit.gen.h"
#include "custom_browser/browser/nexus/tools/glob.gen.h"
#include "custom_browser/browser/nexus/tools/grep.gen.h"
#include "custom_browser/browser/nexus/tools/navigate.gen.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"
#include "custom_browser/browser/nexus/tools/power_shell.gen.h"
#include "custom_browser/browser/nexus/tools/read.gen.h"
#include "custom_browser/browser/nexus/tools/screenshot.gen.h"
#include "custom_browser/browser/nexus/tools/write.gen.h"
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

void ExpectRequiredListEquals(const base::DictValue& schema,
                              std::initializer_list<const char*> expected) {
  const base::ListValue* required = schema.FindList("required");
  ASSERT_NE(required, nullptr);
  ASSERT_EQ(required->size(), expected.size());
  size_t index = 0;
  for (const char* name : expected) {
    EXPECT_EQ((*required)[index].GetString(), name);
    ++index;
  }
}

void ExpectNullableType(const base::DictValue* property,
                        const char* expected_type) {
  ASSERT_NE(property, nullptr);
  const base::ListValue* types = property->FindList("type");
  ASSERT_NE(types, nullptr);
  ASSERT_EQ(types->size(), 2u);
  EXPECT_EQ((*types)[0].GetString(), expected_type);
  EXPECT_EQ((*types)[1].GetString(), "null");
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

TEST(CodegenSmokeTest, ReadSchemaHasStrictVariant) {
  ::nexus::ToolSchema schema = BuildReadSchema();
  EXPECT_EQ(schema.name, "Read");
  EXPECT_TRUE(schema.concurrency_safe);
  ASSERT_EQ(schema.parameters.size(), 4u);
  EXPECT_EQ(schema.parameters[0].name, "file_path");
  EXPECT_TRUE(schema.parameters[0].required);
  EXPECT_EQ(schema.parameters[1].name, "offset");
  EXPECT_FALSE(schema.parameters[1].required);
  EXPECT_EQ(schema.parameters[2].name, "limit");
  EXPECT_FALSE(schema.parameters[2].required);
  EXPECT_EQ(schema.parameters[3].name, "pages");
  EXPECT_FALSE(schema.parameters[3].required);

  ASSERT_TRUE(schema.strict_input_schema.has_value());
  const base::DictValue* properties =
      schema.strict_input_schema->FindDict("properties");
  ASSERT_NE(properties, nullptr);
  EXPECT_NE(properties->FindDict("file_path"), nullptr);
  EXPECT_NE(properties->FindDict("offset"), nullptr);
  EXPECT_NE(properties->FindDict("limit"), nullptr);
  EXPECT_NE(properties->FindDict("pages"), nullptr);
  EXPECT_EQ(properties->FindDict("context_window"), nullptr);
  ExpectRequiredListEquals(*schema.strict_input_schema,
                           {"file_path", "offset", "limit", "pages"});
  const base::DictValue* offset = properties->FindDict("offset");
  ExpectNullableType(offset, "integer");
  const base::DictValue* limit = properties->FindDict("limit");
  ExpectNullableType(limit, "integer");
  const base::DictValue* pages = properties->FindDict("pages");
  ExpectNullableType(pages, "string");
  EXPECT_TRUE(
      schema.strict_input_schema->FindBool("additionalProperties").has_value());
  EXPECT_FALSE(*schema.strict_input_schema->FindBool("additionalProperties"));
}

TEST(CodegenSmokeTest, WriteSchemaHasStrictVariant) {
  ::nexus::ToolSchema schema = BuildWriteSchema();
  EXPECT_EQ(schema.name, "Write");
  ASSERT_TRUE(schema.strict_input_schema.has_value());
  const base::DictValue* properties =
      schema.strict_input_schema->FindDict("properties");
  ASSERT_NE(properties, nullptr);
  EXPECT_NE(properties->FindDict("file_path"), nullptr);
  EXPECT_NE(properties->FindDict("content"), nullptr);

  const base::ListValue* required =
      schema.strict_input_schema->FindList("required");
  ASSERT_NE(required, nullptr);
  ASSERT_EQ(required->size(), 2u);
  EXPECT_EQ((*required)[0].GetString(), "file_path");
  EXPECT_EQ((*required)[1].GetString(), "content");
  EXPECT_TRUE(
      schema.strict_input_schema->FindBool("additionalProperties").has_value());
  EXPECT_FALSE(*schema.strict_input_schema->FindBool("additionalProperties"));
}

TEST(CodegenSmokeTest, EditStrictSchemaUsesProviderCompatibleNullOptionals) {
  ::nexus::ToolSchema schema = BuildEditSchema();
  EXPECT_EQ(schema.name, "Edit");
  ASSERT_TRUE(schema.strict_input_schema.has_value());
  const base::DictValue* properties =
      schema.strict_input_schema->FindDict("properties");
  ASSERT_NE(properties, nullptr);
  EXPECT_NE(properties->FindDict("file_path"), nullptr);
  EXPECT_NE(properties->FindDict("old_string"), nullptr);
  EXPECT_NE(properties->FindDict("new_string"), nullptr);
  EXPECT_NE(properties->FindDict("replace_all"), nullptr);

  ExpectRequiredListEquals(*schema.strict_input_schema,
                           {"file_path", "old_string", "new_string",
                            "replace_all"});
  const base::DictValue* replace_all = properties->FindDict("replace_all");
  ExpectNullableType(replace_all, "boolean");
  EXPECT_TRUE(
      schema.strict_input_schema->FindBool("additionalProperties").has_value());
  EXPECT_FALSE(*schema.strict_input_schema->FindBool("additionalProperties"));
}

TEST(CodegenSmokeTest, ShellStrictSchemasUseProviderCompatibleNullOptionals) {
  for (const auto& schema : {BuildBashSchema(), BuildPowerShellSchema()}) {
    ASSERT_TRUE(schema.strict_input_schema.has_value()) << schema.name;
    const base::DictValue* properties =
        schema.strict_input_schema->FindDict("properties");
    ASSERT_NE(properties, nullptr) << schema.name;
    EXPECT_NE(properties->FindDict("command"), nullptr) << schema.name;
    EXPECT_NE(properties->FindDict("timeout"), nullptr) << schema.name;
    EXPECT_NE(properties->FindDict("description"), nullptr) << schema.name;
    EXPECT_NE(properties->FindDict("run_in_background"), nullptr)
        << schema.name;

    ExpectRequiredListEquals(*schema.strict_input_schema,
                             {"command", "timeout", "description",
                              "run_in_background"});
    ExpectNullableType(properties->FindDict("timeout"), "integer");
    ExpectNullableType(properties->FindDict("description"), "string");
    ExpectNullableType(properties->FindDict("run_in_background"), "boolean");
    const base::DictValue* description = properties->FindDict("description");
    const std::string* description_text =
        description->FindString("description");
    ASSERT_NE(description_text, nullptr) << schema.name;
    if (schema.name == "bash") {
      EXPECT_NE(description_text->find("Never use words like \"complex\""),
                std::string::npos);
    }
    EXPECT_TRUE(schema.strict_input_schema->FindBool("additionalProperties")
                    .has_value())
        << schema.name;
    EXPECT_FALSE(*schema.strict_input_schema->FindBool("additionalProperties"))
        << schema.name;
  }
}

TEST(CodegenSmokeTest, FileSearchSchemasAreConcurrencySafe) {
  EXPECT_TRUE(BuildReadSchema().concurrency_safe);
  EXPECT_TRUE(BuildGlobSchema().concurrency_safe);
  EXPECT_TRUE(BuildGrepSchema().concurrency_safe);
  EXPECT_FALSE(BuildBashSchema().concurrency_safe);
  EXPECT_FALSE(BuildScreenshotSchema().concurrency_safe);
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

TEST(CodegenSmokeTest, ReadExtractTreatsStrictNullOptionalsAsUnset) {
  base::DictValue args;
  args.Set("file_path", "/abs/component.tsx");
  args.Set("offset", base::Value());
  args.Set("limit", base::Value());
  args.Set("pages", base::Value());
  args.Set("context_window", 262144);
  ReadInput input;
  ASSERT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.file_path, "/abs/component.tsx");
  EXPECT_FALSE(input.offset.has_value());
  EXPECT_FALSE(input.limit.has_value());
  EXPECT_FALSE(input.pages.has_value());
  ASSERT_TRUE(input.context_window.has_value());
  EXPECT_EQ(*input.context_window, 262144);
}

TEST(CodegenSmokeTest, EditExtractTreatsStrictNullReplaceAllAsUnset) {
  base::DictValue args;
  args.Set("file_path", "/abs/component.tsx");
  args.Set("old_string", "old");
  args.Set("new_string", "new");
  args.Set("replace_all", base::Value());
  EditInput input;
  ASSERT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.file_path, "/abs/component.tsx");
  EXPECT_EQ(input.old_string, "old");
  EXPECT_EQ(input.new_string, "new");
  EXPECT_FALSE(input.replace_all.has_value());
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

// =============================================================================
// Internal-field omission (`"internal": true` in *.tool.json).
//
// Regression for a Bash failure where the model populated the host-only
// `hook_shell` field on a normal command (`hook_shell: "/bin/bash"`),
// diverting it into the hook-exec path and tripping the shell allowlist
// (`COMMAND_NOT_ALLOWED: tool id '/bin/bash' ...`). The transport fields
// (hook_shell / hook_stdin / hook_env / cwd) are now `internal: true`, so
// codegen keeps them in the typed struct + Extract() (the JS hook runner
// still sends them across the bridge) but OMITS them from the model-facing
// schema — both `schema.parameters` and the strict variant. This mirrors
// Claude Code's BashTool keeping `_simulatedSedEdit` in the input type but
// `.omit()`-ing it from the schema sent to the model.
// =============================================================================

TEST(CodegenSmokeTest, BashSchemaOmitsInternalHostFields) {
  ::nexus::ToolSchema schema = BuildBashSchema();
  EXPECT_EQ(schema.name, "Bash");
  std::set<std::string> param_names;
  for (const auto& p : schema.parameters) {
    param_names.insert(p.name);
  }
  // Model-facing parameters survive.
  EXPECT_TRUE(param_names.count("command"));
  EXPECT_TRUE(param_names.count("timeout"));
  EXPECT_TRUE(param_names.count("description"));
  EXPECT_TRUE(param_names.count("run_in_background"));
  // Host-set transport fields must NOT reach the model-facing schema.
  for (const char* internal_field :
       {"hook_shell", "hook_stdin", "hook_env", "cwd"}) {
    EXPECT_EQ(param_names.count(internal_field), 0u)
        << "internal host-set field '" << internal_field
        << "' leaked into the model-facing Bash schema";
  }
}

TEST(CodegenSmokeTest, BashExtractStillReadsInternalHookFields) {
  // The model-facing schema hides the hook fields, but the JS hook runner
  // still sends them via callTool — Extract() must keep parsing them so the
  // command-hook path (and per-call cwd injection) keeps working.
  base::DictValue args;
  args.Set("command", "echo hi");
  args.Set("hook_shell", "bash");
  args.Set("hook_stdin", "{}");
  args.Set("hook_env", "{\"CLAUDE_PROJECT_DIR\":\"/x\"}");
  args.Set("cwd", "/abs/dir");
  BashInput input;
  ASSERT_FALSE(Extract(args, input).has_value());
  EXPECT_EQ(input.command, "echo hi");
  ASSERT_TRUE(input.hook_shell.has_value());
  EXPECT_EQ(*input.hook_shell, "bash");
  ASSERT_TRUE(input.hook_stdin.has_value());
  ASSERT_TRUE(input.hook_env.has_value());
  ASSERT_TRUE(input.cwd.has_value());
  EXPECT_EQ(*input.cwd, "/abs/dir");
}

TEST(CodegenSmokeTest, PowerShellSchemaOmitsInternalCwd) {
  ::nexus::ToolSchema schema = BuildPowerShellSchema();
  EXPECT_EQ(schema.name, "PowerShell");
  std::set<std::string> param_names;
  for (const auto& p : schema.parameters) {
    param_names.insert(p.name);
  }
  EXPECT_TRUE(param_names.count("command"));
  EXPECT_EQ(param_names.count("cwd"), 0u)
      << "internal host-set 'cwd' leaked into the model-facing "
         "PowerShell schema";
}

TEST(CodegenSmokeTest, GrepSchemaUsesClaudeCodeShortFlagNames) {
  ::nexus::ToolSchema schema = BuildGrepSchema();
  EXPECT_EQ(schema.name, "Grep");
  std::set<std::string> param_names;
  for (const auto& p : schema.parameters) {
    param_names.insert(p.name);
  }
  for (const char* cc_field : {"-A", "-B", "-C", "-n", "-i"}) {
    EXPECT_TRUE(param_names.count(cc_field))
        << "missing CC-compatible Grep field " << cc_field;
  }
  for (const char* old_field :
       {"after_context", "before_context", "show_line_numbers"}) {
    EXPECT_EQ(param_names.count(old_field), 0u)
        << "old Nexus Grep field leaked into model schema: " << old_field;
  }
}

TEST(CodegenSmokeTest, GrepStrictSchemaUsesProviderCompatibleNullOptionals) {
  ::nexus::ToolSchema schema = BuildGrepSchema();
  ASSERT_TRUE(schema.strict_input_schema.has_value());
  const base::DictValue* properties =
      schema.strict_input_schema->FindDict("properties");
  ASSERT_NE(properties, nullptr);
  EXPECT_NE(properties->FindDict("pattern"), nullptr);
  EXPECT_NE(properties->FindDict("output_mode"), nullptr);
  EXPECT_NE(properties->FindDict("head_limit"), nullptr);

  ExpectRequiredListEquals(
      *schema.strict_input_schema,
      {"pattern", "path", "glob", "output_mode", "-B", "-A", "-C",
       "context", "-n", "-i", "type", "head_limit", "offset", "multiline"});

  const base::DictValue* output_mode = properties->FindDict("output_mode");
  ExpectNullableType(output_mode, "string");
  const std::string* output_mode_description =
      output_mode->FindString("description");
  ASSERT_NE(output_mode_description, nullptr);
  EXPECT_NE(output_mode_description->find("\"count\" shows match counts"),
            std::string::npos);
  EXPECT_NE(output_mode_description->find("Use null to select the default"),
            std::string::npos);
  const base::DictValue* head_limit = properties->FindDict("head_limit");
  const std::string* head_limit_description =
      head_limit->FindString("description");
  ASSERT_NE(head_limit_description, nullptr);
  EXPECT_NE(
      head_limit_description->find("Works across all output modes: content"),
      std::string::npos);
  const base::ListValue* modes = output_mode->FindList("enum");
  ASSERT_NE(modes, nullptr);
  ASSERT_EQ(modes->size(), 4u);
  EXPECT_TRUE((*modes)[0].is_none());
  EXPECT_EQ((*modes)[1].GetString(), "files_with_matches");
  EXPECT_EQ((*modes)[2].GetString(), "content");
  EXPECT_EQ((*modes)[3].GetString(), "count");
  ExpectNullableType(properties->FindDict("head_limit"), "integer");
  ExpectNullableType(properties->FindDict("-n"), "boolean");

  EXPECT_TRUE(
      schema.strict_input_schema->FindBool("additionalProperties").has_value());
  EXPECT_FALSE(*schema.strict_input_schema->FindBool("additionalProperties"));
}

TEST(CodegenSmokeTest, GrepExtractReadsShortFlagWireNames) {
  base::DictValue args;
  args.Set("pattern", "TODO");
  args.Set("-A", 2);
  args.Set("-B", 1);
  args.Set("-C", 3);
  args.Set("-n", false);
  args.Set("-i", true);
  GrepInput input;
  ASSERT_FALSE(Extract(args, input).has_value());
  ASSERT_TRUE(input.context_after.has_value());
  EXPECT_EQ(*input.context_after, 2);
  ASSERT_TRUE(input.context_before.has_value());
  EXPECT_EQ(*input.context_before, 1);
  ASSERT_TRUE(input.context_c.has_value());
  EXPECT_EQ(*input.context_c, 3);
  ASSERT_TRUE(input.show_line_numbers.has_value());
  EXPECT_FALSE(*input.show_line_numbers);
  ASSERT_TRUE(input.case_insensitive.has_value());
  EXPECT_TRUE(*input.case_insensitive);
}

}  // namespace
}  // namespace nexus::generated
