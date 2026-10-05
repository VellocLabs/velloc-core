#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_NEXUS_CATALOG_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_NEXUS_CATALOG_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/catalog/catalog_api.gen.h"
#include "custom_browser/browser/nexus/api/shell/host_shell.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `CatalogApiBase` virtuals.
//
// Phase 12 architecture: the bootstrap message previously pushed
// `manifest.hostedTools`; it is pulled from here instead:
//
//   - `listHostedTools` returns the same `HostedToolDefinition[]` the
//     bootstrap message used to embed; sourced from
//     `ToolCatalog::Get().schemas()` and serialized via
//     `BuildHostedToolsList`.
//
//   - `listSkills` serves the hosted skill catalog.
//
// The prompt-section rendering (`renderPromptSection` /
// `listPromptSections`) that used to live here moved to the runtime
// (nexus_web `constants/promptSections/promptSectionRenderer.ts`) and was
// removed under interface 1 on 2026-10-04.
//
// Both actions run on UI (the bridge's home thread) — in-memory work; the
// shell-availability probe behind listHostedTools hops to the thread pool.
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
  void HandleListSkillsOnUI(
      ::nexus::generated::ListSkillsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // UI-thread continuation of HandleListHostedToolsOnUI: filters the static
  // catalog by the off-thread-resolved shell availability and replies.
  void ReplyWithFilteredCatalog(::nexus::BridgeResponseCallback done,
                                HostShellAvailability availability);

  base::WeakPtrFactory<NexusCatalogBridgeAdapter> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_NEXUS_CATALOG_BRIDGE_ADAPTER_H_
