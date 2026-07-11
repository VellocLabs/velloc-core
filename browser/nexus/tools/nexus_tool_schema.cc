#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

#include <utility>

namespace nexus {

namespace {

// Builds the per-property dict for one declared parameter.
// Layout: { "type": "<json schema type>", "description": "...", ... }
// Array kinds also emit `"items": { "type": "<element type>" }` so the
// LLM-facing schema is closed over the element type. The `description`
// field flows straight from the macro's `NEXUS_INPUT(kind, name, desc)`
// third argument and is what the model reads to understand the slot.
base::DictValue BuildPropertyDict(const ToolParameter& param) {
  base::DictValue prop;
  prop.Set("type", JsonSchemaTypeString(param.kind));
  if (!param.description.empty()) {
    prop.Set("description", param.description);
  }
  switch (param.kind) {
    case JsonSchemaKind::kArrayOfString: {
      base::DictValue items;
      items.Set("type", "string");
      prop.Set("items", std::move(items));
      break;
    }
    case JsonSchemaKind::kArrayOfInteger: {
      base::DictValue items;
      items.Set("type", "integer");
      prop.Set("items", std::move(items));
      break;
    }
    case JsonSchemaKind::kArrayOfNumber: {
      base::DictValue items;
      items.Set("type", "number");
      prop.Set("items", std::move(items));
      break;
    }
    case JsonSchemaKind::kArrayOfObject: {
      base::DictValue items;
      items.Set("type", "object");
      if (param.name == "actions") {
        base::DictValue item_properties;
        base::DictValue type_property;
        type_property.Set("type", "string");
        type_property.Set("description",
                          "Simple browser action type, e.g. click, input, "
                          "navigate, search, wait, read_page, find_text, "
                          "or done.");
        item_properties.Set("type", std::move(type_property));

        base::ListValue item_required;
        item_required.Append("type");
        items.Set("properties", std::move(item_properties));
        items.Set("required", std::move(item_required));
        prop.Set("minItems", 1);
        prop.Set("maxItems", 5);
      }
      prop.Set("items", std::move(items));
      break;
    }
    default:
      // Scalars / kObject / kAny — no items clause.
      break;
  }
  return prop;
}

}  // namespace

// =============================================================================
// ToolParameter / ToolSchema — out-of-line ctors / dtors required by
// chromium-style on structs with multiple non-trivial members.
// =============================================================================

ToolParameter::ToolParameter() = default;
ToolParameter::ToolParameter(std::string name,
                             std::string description,
                             JsonSchemaKind kind,
                             bool required)
    : name(std::move(name)),
      description(std::move(description)),
      kind(kind),
      required(required) {}
ToolParameter::ToolParameter(const ToolParameter&) = default;
ToolParameter::ToolParameter(ToolParameter&&) noexcept = default;
ToolParameter& ToolParameter::operator=(const ToolParameter&) = default;
ToolParameter& ToolParameter::operator=(ToolParameter&&) noexcept = default;
ToolParameter::~ToolParameter() = default;

ToolSchema::ToolSchema() = default;
ToolSchema::ToolSchema(const ToolSchema&) = default;
ToolSchema::ToolSchema(ToolSchema&&) noexcept = default;
ToolSchema& ToolSchema::operator=(const ToolSchema&) = default;
ToolSchema& ToolSchema::operator=(ToolSchema&&) noexcept = default;
ToolSchema::~ToolSchema() = default;

// =============================================================================
// JSON-Schema helpers
// =============================================================================

const char* JsonSchemaTypeString(JsonSchemaKind kind) {
  switch (kind) {
    case JsonSchemaKind::kString:
      return "string";
    case JsonSchemaKind::kInteger:
      return "integer";
    case JsonSchemaKind::kNumber:
      return "number";
    case JsonSchemaKind::kBoolean:
      return "boolean";
    case JsonSchemaKind::kArrayOfString:
    case JsonSchemaKind::kArrayOfInteger:
    case JsonSchemaKind::kArrayOfNumber:
    case JsonSchemaKind::kArrayOfObject:
      return "array";
    case JsonSchemaKind::kObject:
      return "object";
    case JsonSchemaKind::kAny:
      // The LLM-facing wire format requires *some* type. "object" is the
      // most permissive — the tool body validates the actual shape.
      return "object";
  }
  return "object";
}

base::DictValue BuildHostedToolDefinitionDict(const ToolSchema& schema) {
  base::DictValue properties;
  base::ListValue required;
  for (const ToolParameter& param : schema.parameters) {
    properties.Set(param.name, BuildPropertyDict(param));
    if (param.required) {
      required.Append(param.name);
    }
  }

  base::DictValue parameters;
  parameters.Set("type", "object");
  parameters.Set("properties", std::move(properties));
  if (!required.empty()) {
    parameters.Set("required", std::move(required));
  }

  base::DictValue tool;
  tool.Set("type", "function");
  tool.Set("name", schema.name);
  tool.Set("description", schema.description);
  tool.Set("parameters", std::move(parameters));
  return tool;
}

base::ListValue BuildHostedToolsList(base::span<const ToolSchema> schemas) {
  base::ListValue list;
  for (const ToolSchema& schema : schemas) {
    list.Append(BuildHostedToolDefinitionDict(schema));
  }
  return list;
}

}  // namespace nexus
