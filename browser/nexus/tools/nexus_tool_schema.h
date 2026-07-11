#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SCHEMA_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SCHEMA_H_

#include <cstdint>
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
