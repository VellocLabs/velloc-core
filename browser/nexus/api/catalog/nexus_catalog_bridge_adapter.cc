#include "custom_browser/browser/nexus/api/catalog/nexus_catalog_bridge_adapter.h"

#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_section_renderer.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"
#include "custom_browser/browser/nexus/tools/tool_catalog.h"

namespace nexus {

NexusCatalogBridgeAdapter::NexusCatalogBridgeAdapter(
    NexusBridgeResponseDelegate* delegate)
    : ::nexus::generated::CatalogApiBase(delegate) {
  // Register(...) calls live in `CatalogApiBase`'s ctor, generated
  // from api/declarations/catalog/*.api.json. Adding a new catalog
  // wire action: drop a *.api.json under api/declarations/catalog/,
  // add a Handle*OnUI() override here. The codegen wires the
  // dispatch automatically.
}

NexusCatalogBridgeAdapter::~NexusCatalogBridgeAdapter() = default;

void NexusCatalogBridgeAdapter::HandleListHostedToolsOnUI(
    ::nexus::generated::ListHostedToolsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Same wire format as the pre-Phase-12 bootstrap manifest's
  // `hostedTools` field — `BuildHostedToolsList` is the existing
  // serializer in `tools/nexus_tool_schema.cc`, reused unchanged.
  base::ListValue tools = BuildHostedToolsList(ToolCatalog::Get().schemas());
  base::DictValue result;
  result.Set("hostedTools", std::move(tools));
  std::move(done).Success(std::move(result));
}

void NexusCatalogBridgeAdapter::HandleRenderPromptSectionOnUI(
    ::nexus::generated::RenderPromptSectionInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Unknown section names fall through to an empty string per the
  // renderer's contract — matches the TS behavior where an
  // unsupported placeholder leaves the slot empty.
  std::string text = RenderPromptSection(input.name, input.enabledToolNames);

  base::DictValue result;
  result.Set("text", std::move(text));
  std::move(done).Success(std::move(result));
}

void NexusCatalogBridgeAdapter::HandleListPromptSectionsOnUI(
    ::nexus::generated::ListPromptSectionsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ListValue sections;
  for (const std::string& name : ListPromptSectionNames()) {
    sections.Append(name);
  }

  base::DictValue result;
  result.Set("sections", std::move(sections));
  std::move(done).Success(std::move(result));
}

}  // namespace nexus
