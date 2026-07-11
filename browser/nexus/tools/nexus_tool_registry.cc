#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"

#include <utility>

#include "base/logging.h"

namespace nexus {

// =============================================================================
// ToolEntry — out-of-line ctor / dtor required by chromium-style
// =============================================================================

ToolEntry::ToolEntry() = default;
ToolEntry::ToolEntry(ToolSchema schema, ToolDispatcher dispatcher)
    : schema(std::move(schema)), dispatcher(std::move(dispatcher)) {}
ToolEntry::ToolEntry(ToolEntry&&) noexcept = default;
ToolEntry& ToolEntry::operator=(ToolEntry&&) noexcept = default;
ToolEntry::~ToolEntry() = default;

// =============================================================================
// ToolBuilder
// =============================================================================

ToolBuilder::ToolBuilder(std::vector<ToolEntry>* sink,
                         std::string name,
                         std::string description)
    : sink_(sink) {
  schema_.name = std::move(name);
  schema_.description = std::move(description);
}

ToolBuilder::~ToolBuilder() {
  // Catch the easy mistake of forgetting `.Bind(...)`. The schema is
  // useless without a dispatcher and would silently disappear; loud-fail
  // instead so the developer notices during local testing.
  if (!committed_) {
    LOG(DFATAL) << "[NexusToolBuilder] Tool '" << schema_.name
                << "' was declared but never bound to a dispatcher. "
                   "Did you forget the trailing .Bind(...)?";
  }
}

ToolBuilder::ToolBuilder(ToolBuilder&& other) noexcept
    : sink_(other.sink_),
      schema_(std::move(other.schema_)),
      committed_(other.committed_) {
  other.sink_ = nullptr;
  other.committed_ = true;  // suppress moved-from destructor's DFATAL
}

ToolBuilder& ToolBuilder::Required(std::string name,
                                   std::string description,
                                   JsonSchemaKind kind) {
  schema_.parameters.emplace_back(std::move(name), std::move(description),
                                  kind, /*required=*/true);
  return *this;
}

ToolBuilder& ToolBuilder::Optional(std::string name,
                                   std::string description,
                                   JsonSchemaKind kind) {
  schema_.parameters.emplace_back(std::move(name), std::move(description),
                                  kind, /*required=*/false);
  return *this;
}

ToolBuilder& ToolBuilder::Returns(JsonSchemaKind kind) {
  schema_.output_kind = kind;
  return *this;
}

ToolBuilder& ToolBuilder::Skill(std::string skill) {
  schema_.skill = std::move(skill);
  return *this;
}

ToolBuilder& ToolBuilder::ConcurrencySafe(bool concurrency_safe) {
  schema_.concurrency_safe = concurrency_safe;
  return *this;
}

ToolBuilder& ToolBuilder::StrictInputSchema(base::DictValue schema) {
  schema_.strict_input_schema = std::move(schema);
  return *this;
}

ToolBuilder& ToolBuilder::InputExamples(base::ListValue examples) {
  schema_.input_examples = std::move(examples);
  return *this;
}

void ToolBuilder::Bind(ToolDispatcher dispatcher) && {
  CHECK(sink_) << "Bind called twice or on a moved-from ToolBuilder";
  sink_->emplace_back(std::move(schema_), std::move(dispatcher));
  committed_ = true;
  sink_ = nullptr;
}

// =============================================================================
// ToolRegistry
// =============================================================================

ToolRegistry::ToolRegistry() = default;
ToolRegistry::~ToolRegistry() = default;

ToolBuilder ToolRegistry::Tool(std::string name, std::string description) {
  return ToolBuilder(&entries_, std::move(name), std::move(description));
}

std::vector<ToolSchema> ToolRegistry::CollectSchemas() const {
  std::vector<ToolSchema> schemas;
  schemas.reserve(entries_.size());
  for (const ToolEntry& entry : entries_) {
    schemas.push_back(entry.schema);
  }
  return schemas;
}

}  // namespace nexus
