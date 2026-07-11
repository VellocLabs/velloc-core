#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_NEXUS_UI_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_NEXUS_UI_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/ui/ui_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `UiApiBase` virtuals.
//
// Phase 11h architecture (mirrors api/llm — UI thread + typed inputs +
// BridgeResponseCallback):
//   - 8 actions declared under api/declarations/ui/*.api.json. All run
//     on the bridge's home thread (UI) — they touch BrowserView / WebUI
//     directly which is UI-bound.
//   - The codegen-emitted dispatcher extracts a typed input struct
//     and constructs `BridgeResponseCallback done`. The implementation
//     does its UI work and fires `done.Success/Error/InvalidArgs`.
//   - The two `broadcast*` actions use `passthrough_payload` because
//     the payload shape is opaque — they re-broadcast it verbatim as
//     a WebUI listener event.
//
// Helpers like `BroadcastWebUiListenerEventToProfile` live at
// `nexus/ui/nexus_ui_broadcast.{h,cc}` (kept where they are; only the
// adapter moved under api/).
class NexusUiBridgeAdapter : public ::nexus::generated::UiApiBase {
 public:
  NexusUiBridgeAdapter(NexusBridgeResponseDelegate* delegate, Profile* profile);
  ~NexusUiBridgeAdapter() override;

  NexusUiBridgeAdapter(const NexusUiBridgeAdapter&) = delete;
  NexusUiBridgeAdapter& operator=(const NexusUiBridgeAdapter&) = delete;

 private:
  // Overrides of generated::UiApiBase pure virtuals.

  void HandleOpenNexusSettingsOnUI(
      ::nexus::generated::OpenNexusSettingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleGetProviderMissingPromptOnUI(
      ::nexus::generated::GetProviderMissingPromptInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleDismissBootWelcomeOnUI(
      ::nexus::generated::DismissBootWelcomeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleShowMainContentsOnUI(
      ::nexus::generated::ShowMainContentsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleOpenUrlOnUI(
      ::nexus::generated::OpenUrlInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleGetBootTimingsOnUI(
      ::nexus::generated::GetBootTimingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleBroadcastCurrentSessionStateOnUI(
      ::nexus::generated::BroadcastCurrentSessionStateInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleBroadcastSessionsSummaryOnUI(
      ::nexus::generated::BroadcastSessionsSummaryInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<Profile> profile_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_NEXUS_UI_BRIDGE_ADAPTER_H_
