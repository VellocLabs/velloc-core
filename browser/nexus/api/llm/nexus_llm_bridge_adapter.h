#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/llm/llm_api.gen.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_support.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusLlmHost;

// Concrete adapter implementing the generated `LlmApiBase` virtuals.
//
// Phase 10f architecture (see fs adapter for the full description):
//   - Per *.api.json declares its inputs in tools-style `inputs[]`.
//   - LLM actions all use `runtime.thread = "ui"` and the
//     `passthrough_payload` pseudo-type for the request payload (the
//     inner shape is provider-translated and not worth enumerating
//     per-field on the bridge wire).
//   - Each Handle*OnUI runs on the bridge's home thread (UI). It reads
//     fields off `input.payload`, invokes the relevant llm_host_ async
//     method, and forwards the result through `done`. The
//     BridgeResponseCallback handles UI-thread marshaling internally
//     so the implementation never PostTasks for the response hop.
class NexusLlmBridgeAdapter : public ::nexus::generated::LlmApiBase {
 public:
  NexusLlmBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                        Profile* profile);
  ~NexusLlmBridgeAdapter() override;

  NexusLlmBridgeAdapter(const NexusLlmBridgeAdapter&) = delete;
  NexusLlmBridgeAdapter& operator=(const NexusLlmBridgeAdapter&) = delete;

  // Pushes a `session.applySelection` event to nexus_web. Called by the
  // settings handler when the user picks a model in the WebUI quick-
  // settings popup. nexus_web responds by rebinding the active session
  // and re-publishing its status (which loops back here as a normal
  // request via HandlePublishStatusOnUI).
  void PushSessionApplySelection(const std::string& request_id,
                                 const std::string& provider_id,
                                 const std::string& model_id,
                                 const std::string& thinking_tier,
                                 bool also_update_default);

 private:
  // Overrides of generated::LlmApiBase pure virtuals. All run on UI by
  // contract (`runtime.thread = "ui"` in every llm/*.api.json).

  void HandleCatalogOnUI(
      ::nexus::generated::CatalogInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleCallOnUI(
      ::nexus::generated::CallInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStreamStartOnUI(
      ::nexus::generated::StreamStartInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStreamReadOnUI(
      ::nexus::generated::StreamReadInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStreamCancelOnUI(
      ::nexus::generated::StreamCancelInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetDefaultSelectionOnUI(
      ::nexus::generated::SetDefaultSelectionInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePublishCatalogOnUI(
      ::nexus::generated::PublishCatalogInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePublishProviderConfigsOnUI(
      ::nexus::generated::PublishProviderConfigsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRegisterToolDefsOnUI(
      ::nexus::generated::RegisterToolDefsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePublishBinaryRegistryOnUI(
      ::nexus::generated::PublishBinaryRegistryInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // `model.encryptSecret`: seals a provider api key with the OS keystore so
  // the guest persists only ciphertext in settings.json. Waits for the
  // encryptor if it is not ready yet; never answers with plaintext.
  void HandleEncryptSecretOnUI(
      ::nexus::generated::EncryptSecretInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // Phase 12f: `publishStatus` moved out of `llm/` into the new
  // `session/` capability — see
  // `api/session/nexus_session_bridge_adapter.{h,cc}`. The wire
  // contract is unchanged; only the adapter that handles it changed.

  raw_ptr<Profile> profile_;
  std::unique_ptr<NexusLlmHost> llm_host_;
  std::map<std::string, NexusLlmBridgeStreamState> bridge_stream_states_;
  // Token returned by `LlmBroker::AddPickEventPusher`. Stored so we
  // can remove our pusher in the destructor without leaving a dangling
  // entry in the broker's fan-out map.
  std::optional<int> pick_event_pusher_token_;
  base::WeakPtrFactory<NexusLlmBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
