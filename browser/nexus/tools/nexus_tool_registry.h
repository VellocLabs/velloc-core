#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_REGISTRY_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

// `nexus_tool_registry.h` — framework primitives shared by every tool
// adapter:
//   - `ToolSchema` / `ToolParameter`         (in nexus_tool_schema.h)
//   - `ToolCallback`                         (in nexus_tool_callback.h)
//   - `ToolBuilder` / `ToolRegistry`         (defined below)
//   - `ToolEntry` { schema, dispatcher }     (defined below)
//
// The macro DSL (`NEXUS_TOOL` / `NEXUS_INPUT` / `NEXUS_END_TOOL` —
// see nexus_tool_macros.h) populates a `ToolRegistry`; the global
// `ToolCatalog` (`tool_catalog.h`) owns the registry, exposes its
// schemas to the manifest builder, and hands its entries to
// `ToolsAdapter` (`tools_adapter.h`) — which wraps each entry with a
// session-routing bridge handler.

namespace nexus {

class ToolSession;

// One declared tool's runtime entry: schema (drives the manifest entry
// and parameter validation) + dispatcher (called per request to invoke
// the tool body with the decoded args and a fresh `ToolCallback`).
//
// `ToolDispatcher` runs on the same sequence as the bridge port service
// (currently the UI thread); the tool body is responsible for hopping
// elsewhere if needed and resolving the callback when ready.
//
// The first parameter is the `ToolSession` the request is being routed
// to. The dispatcher's inner trampoline (built by the macro DSL via
// `BindTypedCategoryMethod`) descends to the matching per-category
// member via `CategoryAccessor<Category>::Get(session)` before
// invoking the typed tool method.
using ToolDispatcher = base::RepeatingCallback<void(
    ToolSession* session, base::DictValue args, ToolCallback done)>;

struct ToolEntry {
  ToolEntry();
  ToolEntry(ToolSchema schema, ToolDispatcher dispatcher);
  ToolEntry(ToolEntry&&) noexcept;
  ToolEntry& operator=(ToolEntry&&) noexcept;
  ToolEntry(const ToolEntry&) = delete;
  ToolEntry& operator=(const ToolEntry&) = delete;
  ~ToolEntry();

  ToolSchema schema;
  ToolDispatcher dispatcher;
};

// Fluent builder for one tool. Returned by `ToolRegistry::Tool(...)`.
// Chain `.Required*` / `.Optional*` / `.Returns*` and finish with
// `.Bind(...)` (or any of the typed `BindTo*` helpers exposed by the
// macro layer).
class ToolBuilder {
 public:
  ToolBuilder(std::vector<ToolEntry>* sink,
              std::string name,
              std::string description);
  ~ToolBuilder();

  ToolBuilder(const ToolBuilder&) = delete;
  ToolBuilder& operator=(const ToolBuilder&) = delete;

  ToolBuilder(ToolBuilder&&) noexcept;
  ToolBuilder& operator=(ToolBuilder&&) = delete;

  // Add one parameter slot. `description` is shipped to the model as
  // the JSON-Schema `description` field for this property and is what
  // the LLM reads to understand the slot. Use the empty string only if
  // the parameter is fully self-explanatory from its name.
  ToolBuilder& Required(std::string name,
                        std::string description,
                        JsonSchemaKind kind);
  ToolBuilder& Optional(std::string name,
                        std::string description,
                        JsonSchemaKind kind);

  // Output info — informational; not currently emitted in the wire
  // format but reserved for future structured-output hints. Set by the
  // `NEXUS_END_TOOL(<kind>)` macro.
  ToolBuilder& Returns(JsonSchemaKind kind);

  // Attach a hand-crafted strict-tool-use schema variant. Optional;
  // when set, `BuildHostedToolDefinitionDict` emits it as
  // `parametersStrict` on the hosted-tool entry alongside the
  // permissive `parameters` block. nexus_web sends this variant +
  // `strict: true` when the active model's catalog flag
  // `supportsStrictTools` is true.
  //
  // Author owns strict-compliance: the schema must satisfy each
  // provider's accepted JSON Schema subset (no `maxItems` /
  // `minItems` / `pattern` / `format` etc. — see ToolSchema's
  // `strict_input_schema` doc for the full list).
  ToolBuilder& StrictInputSchema(base::DictValue schema);

  // Attach canonical example calls. Each entry must be a dict that
  // validates against the tool's `input_schema`. When set,
  // `BuildHostedToolDefinitionDict` emits these as `inputExamples` on
  // the hosted-tool entry; the Anthropic provider then forwards them
  // as `input_examples` on the wire (other providers silently drop).
  // Use to anchor structurally-tricky tool calls when strict mode is
  // either too expensive (grammar compilation) or too restrictive.
  ToolBuilder& InputExamples(base::ListValue examples);

  // Bind the dispatcher and commit the entry to the registry. Idiomatic
  // call sites build a dispatcher via one of the `BindTo*` helpers in
  // nexus_tool_macros.h that wraps a member function pointer.
  void Bind(ToolDispatcher dispatcher) &&;

 private:
  std::vector<ToolEntry>* sink_;
  ToolSchema schema_;
  bool committed_ = false;
};

// Collects ToolEntry instances. One registry per adapter; populated in
// the adapter's `DefineTools(ToolRegistry&)` override and consumed
// during `RegisterAllTools()`.
class ToolRegistry {
 public:
  ToolRegistry();
  ~ToolRegistry();

  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  // Begin a new tool. Chain the result via the typed builder methods
  // and finish with `.Bind(...)`.
  ToolBuilder Tool(std::string name, std::string description);

  // Read-only views.
  const std::vector<ToolEntry>& entries() const { return entries_; }
  std::vector<ToolSchema> CollectSchemas() const;

 private:
  std::vector<ToolEntry> entries_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_REGISTRY_H_
