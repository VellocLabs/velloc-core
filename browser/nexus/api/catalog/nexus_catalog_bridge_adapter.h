#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_NEXUS_CATALOG_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_NEXUS_CATALOG_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/catalog/catalog_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Which host shells resolved on PATH. Computed off the UI thread (PATH
// stats are blocking I/O) and used to gate the Bash / PowerShell tools out
// of the listHostedTools response on hosts that can't run them.
struct CatalogShellAvailability {
  bool bash = false;
  bool pwsh = false;
};

// Concrete adapter implementing the generated `CatalogApiBase` virtuals.
//
// Phase 12 architecture: the bootstrap message previously pushed
// `manifest.hostedTools` and the prompt-section rendering ran in TS via
// `getUsingYourToolsSection`. Both have moved here:
//
//   - `listHostedTools` returns the same `HostedToolDefinition[]` the
//     bootstrap message used to embed; sourced from
//     `ToolCatalog::Get().schemas()` and serialized via
//     `BuildHostedToolsList`.
//
//   - `renderPromptSection` walks the catalog + the JS-supplied enabled-
//     tool set and produces the prose that fills `{{TOOL_CATALOG}}` in
//     the .md template. The C++ rendering replaces the hand-maintained
//     `BROWSER_TOOL_NAMES` set in
//     `nexus_web/src/constants/systemPromptToolSection.ts`.
//
//   - `listPromptSections` advertises which placeholder names the
//     guest can ask for.
//
// All three actions run on UI (the bridge's home thread) — pure
// in-memory work, no blocking I/O.
class NexusCatalogBridgeAdapter
    : public ::nexus::generated::CatalogApiBase {
 public:
  explicit NexusCatalogBridgeAdapter(NexusBridgeResponseDelegate* delegate);
  ~NexusCatalogBridgeAdapter() override;

  NexusCatalogBridgeAdapter(const NexusCatalogBridgeAdapter&) = delete;
  NexusCatalogBridgeAdapter& operator=(const NexusCatalogBridgeAdapter&) =
      delete;

 private:
  void HandleListHostedToolsOnUI(
      ::nexus::generated::ListHostedToolsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRenderPromptSectionOnUI(
      ::nexus::generated::RenderPromptSectionInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleListPromptSectionsOnUI(
      ::nexus::generated::ListPromptSectionsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // UI-thread continuation of HandleListHostedToolsOnUI: filters the static
  // catalog by the off-thread-resolved shell availability and replies.
  void ReplyWithFilteredCatalog(::nexus::BridgeResponseCallback done,
                                CatalogShellAvailability availability);

  base::WeakPtrFactory<NexusCatalogBridgeAdapter> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_NEXUS_CATALOG_BRIDGE_ADAPTER_H_
