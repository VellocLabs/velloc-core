#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SCHEMA_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SCHEMA_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/values.h"

// Tool descriptor types for the nexus C++-side tool registry.
//
// Adapters declare browser-actor tools via `ToolRegistry` (see
// `nexus_tool_registry.h`) or the `NEXUS_TOOL` macro DSL (see
// `nexus_tool_macros.h`). Every declared tool produces a `ToolSchema`
// that:
//   1. Becomes part of the bootstrap manifest's `hostedTools` list, which
//      the LLM consumes as its tool catalog;
//   2. Drives runtime parameter extraction in the dispatcher; and
//   3. Is the single source of truth for the tool's name + description.
//
// The wire format mirrors `HostedToolDefinition` in
// `nexus_web/src/browser/bridge/protocol/manifest.ts` so the JS guest
// reads it without translation.
//
// Per-parameter descriptions are intentionally NOT carried — the tool's
// top-level `description` is the place to explain every parameter, what
// they mean, and how they interact. Keeping descriptions in one block
// reads better for the LLM and avoids the temptation to split rationale
// across multiple short fields.

namespace nexus {

// Coarse JSON-Schema kind. Mirrors what the LLM-facing schema needs;
// finer-grained validation (length, enum) is handled in the tool body
// for now. `kArrayOfString`, etc., are the array variants we currently
// emit; anything more exotic is `kAny` and the tool body validates it.
enum class JsonSchemaKind : uint8_t {
  kString,
  kInteger,
  kNumber,
  kBoolean,
  kArrayOfString,
  kArrayOfInteger,
  kArrayOfNumber,
  kArrayOfObject,
  kObject,
  kAny,
};

// One declared parameter on a tool — name, model-facing description,
// expected JSON kind, and required/optional flag. Stored by value
// because the catalog is built once at adapter construction and
// immutable thereafter.
struct ToolParameter {
  ToolParameter();
  ToolParameter(std::string name,
                std::string description,
                JsonSchemaKind kind,
                bool required);
  ToolParameter(const ToolParameter&);
  ToolParameter(ToolParameter&&) noexcept;
  ToolParameter& operator=(const ToolParameter&);
  ToolParameter& operator=(ToolParameter&&) noexcept;
  ~ToolParameter();

  std::string name;
  std::string description;
  JsonSchemaKind kind = JsonSchemaKind::kAny;
  bool required = false;
};

// One declared tool — name (the LLM sees this verbatim), description,
// and ordered parameter list. The dispatcher and the manifest entry are
// both derived from this struct.
//
// Chromium-style requires explicit out-of-line ctors / dtor on structs
// with non-trivial members (the multiple std::string + std::vector
// members below trip the rule).
struct ToolSchema {
  ToolSchema();
  ToolSchema(const ToolSchema&);
  ToolSchema(ToolSchema&&) noexcept;
  ToolSchema& operator=(const ToolSchema&);
  ToolSchema& operator=(ToolSchema&&) noexcept;
  ~ToolSchema();

  std::string name;
  std::string description;
  std::vector<ToolParameter> parameters;
  // Output kind is informational; current `HostedToolDefinition` wire
  // format does not encode the response schema explicitly. Kept here so
  // a future iteration can surface it without another descriptor change.
  JsonSchemaKind output_kind = JsonSchemaKind::kAny;
  // Optional hand-crafted JSON Schema variant for strict tool use
  // (Anthropic / OpenAI / GLM grammar-constrained sampling). When
  // present, BuildHostedToolDefinitionDict emits it as
  // `parametersStrict` on the hosted-tool entry alongside the
  // permissive `parameters` block.
  //
  // The author is responsible for keeping this strict-compliant per
  // each provider's accepted JSON Schema subset:
  //   - `additionalProperties: false` on every object
  //   - no `maxItems` / `minItems` / `maxLength` / `minLength` /
  //     `pattern` / `format` (Anthropic strict subset rejects these;
  //     OpenAI accepts a slightly larger set but the safest
  //     intersection is the Anthropic one)
  //   - the runtime extractor still does per-action-type validation
  //     (see browser_tool.cc), so the strict schema only needs to
  //     enforce gross shape, not per-type required fields.
  //
  // The C++ JS guest (nexus_web) gates strict-mode use by the active
  // model's `supportsStrictTools` catalog flag; tools that don't
  // populate this field silently skip strict mode regardless of model.
  std::optional<base::DictValue> strict_input_schema;

  // Optional canonical example calls — each entry is a `base::Value`
  // dict matching the tool's `input_schema`. When present,
  // BuildHostedToolDefinitionDict emits these as `inputExamples` on
  // the hosted-tool entry. The JS guest forwards them to the per-
  // provider translator; only Anthropic surfaces them on the wire
  // (as `input_examples`) — OpenAI / Google / Ollama ignore.
  //
  // Use this to anchor structurally-tricky tool calls. For
  // browser_actions specifically, examples that show all four required
  // top-level fields populated AND a real `actions[]` batch are the
  // strongest defense against the empty-args failure mode without
  // paying strict-mode's grammar-compilation cost.
  //
  // Each example MUST validate against `input_schema`; Anthropic
  // returns 400 on any violation.
  std::optional<base::ListValue> input_examples;
};

// Returns the JSON-Schema string used in the wire format for a kind.
// E.g. kString -> "string", kArrayOfInteger -> "array".
const char* JsonSchemaTypeString(JsonSchemaKind kind);

// Builds the per-tool dict that goes into `manifest.hostedTools`. Layout
// matches HostedToolDefinition on the JS side:
//   { type: "function", name, description,
//     parameters: { type: "object", properties: {...}, required: [...] } }
base::DictValue BuildHostedToolDefinitionDict(const ToolSchema& schema);

// Builds the `hostedTools` array from a span of schemas. Convenience
// wrapper used by `nexus_host_manifest.cc`.
base::ListValue BuildHostedToolsList(base::span<const ToolSchema> schemas);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SCHEMA_H_
