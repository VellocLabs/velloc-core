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
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_support.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_raw_stream.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusLlmHost;

// Concrete adapter implementing the generated `LlmApiBase` virtuals. All
// actions run on UI: the host serves the catalog, the provider configs and
// the `rawStream*` byte transport; replies go through `done`.
class NexusLlmBridgeAdapter : public ::nexus::generated::LlmApiBase {
 public:
  NexusLlmBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                        Profile* profile);
  ~NexusLlmBridgeAdapter() override;

  NexusLlmBridgeAdapter(const NexusLlmBridgeAdapter&) = delete;
  NexusLlmBridgeAdapter& operator=(const NexusLlmBridgeAdapter&) = delete;

  // Pushes a `session.applySelection` event to nexus_web. Registered as a
  // `LlmBroker` pick-event pusher; runs on every
  // `LlmBroker::DispatchUserPick` (the C++ `providers.setActive` route).
  // Inert today: the guest no longer subscribes to this event.
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
  // `model.rawStream{Open,Read,Cancel}`: a provider-scoped byte transport
  // for providers whose wire protocol the guest implements (see
  // NexusLlmRawStream). The host resolves the provider's active base URL
  // and stored credential, injects the credential, and confines the
  // request to the provider's domain — the guest never sees the secret.
  void HandleRawStreamOpenOnUI(
      ::nexus::generated::RawStreamOpenInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // The body of rawStreamOpen, after any `{{oauth:NAME}}` values were
  // resolved (nullopt: none were needed).
  void ContinueRawStreamOpen(::nexus::generated::RawStreamOpenInput input,
                             ::nexus::BridgeResponseCallback done,
                             std::optional<OAuthValues> oauth);
  void HandleRawStreamReadOnUI(
      ::nexus::generated::RawStreamReadInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRawStreamCancelOnUI(
      ::nexus::generated::RawStreamCancelInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // Buffered-mode transform for a request with `captureSecrets`: stores the
  // captured values (sealed) under the provider's `secrets` and returns the
  // redacted body plus `captured` (sealed copies) for the final read.
  NexusLlmRawStream::TransformResult CaptureSecrets(
      const std::string& provider_id,
      const std::vector<SecretCapture>& captures,
      std::string body,
      int status);
  void OnRawStreamOpened(std::string stream_id,
                         ::nexus::BridgeResponseCallback done,
                         NexusLlmRawStream::OpenResult result);
  // Deferred erase: a stream may report its end from inside its own
  // callbacks, so it is never deleted synchronously from them.
  void EraseRawStreamSoon(const std::string& stream_id);

  raw_ptr<Profile> profile_;
  std::unique_ptr<NexusLlmHost> llm_host_;
  // Open raw streams by id. Bounded by kMaxRawStreams (the .cc).
  std::map<std::string, std::unique_ptr<NexusLlmRawStream>> raw_streams_;
  // Ids a rawStreamCancel named before their rawStreamOpen was handled.
  RecentIdSet cancelled_raw_stream_ids_;
  // Token returned by `LlmBroker::AddPickEventPusher`. Stored so we
  // can remove our pusher in the destructor without leaving a dangling
  // entry in the broker's fan-out map.
  std::optional<int> pick_event_pusher_token_;
  base::WeakPtrFactory<NexusLlmBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
