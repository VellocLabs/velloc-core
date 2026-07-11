#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/llm/nexus_llm_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"

class Profile;

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusLlmHost;

class NexusLlmBridgeAdapter : public NexusBridgeCapabilityHandler {
 public:
  NexusLlmBridgeAdapter(NexusBridgeResponseDelegate* delegate, Profile* profile);
  ~NexusLlmBridgeAdapter() override;

  NexusLlmBridgeAdapter(const NexusLlmBridgeAdapter&) = delete;
  NexusLlmBridgeAdapter& operator=(const NexusLlmBridgeAdapter&) = delete;

  // Pushes a `session.applySelection` event to nexus_web. Called by the
  // settings handler when the user picks a model in the WebUI quick-
  // settings popup. nexus_web responds by rebinding the active session
  // and re-publishing its status (which loops back here as a normal
  // request via HandleSessionPublishStatus).
  void PushSessionApplySelection(const std::string& request_id,
                                 const std::string& provider_id,
                                 const std::string& model_id,
                                 const std::string& thinking_tier,
                                 bool also_update_default);

 private:
  void HandleCatalog(const std::string& id,
                     const base::DictValue& route,
                     const base::DictValue& payload,
                     std::vector<blink::MessagePortDescriptor> ports);
  void HandleCall(const std::string& id,
                  const base::DictValue& route,
                  const base::DictValue& payload,
                  std::vector<blink::MessagePortDescriptor> ports);
  void HandleStreamStart(const std::string& id,
                         const base::DictValue& route,
                         const base::DictValue& payload,
                         std::vector<blink::MessagePortDescriptor> ports);
  void HandleStreamRead(const std::string& id,
                        const base::DictValue& route,
                        std::string stream_id,
                        std::vector<blink::MessagePortDescriptor> ports);
  void HandleSetDefaultSelection(
      const std::string& id,
      const base::DictValue& route,
      std::string provider_id,
      std::string model_id,
      std::vector<blink::MessagePortDescriptor> ports);
  // nexus_web → C++ : pushes the curated NEXUS_MODEL_CONFIG snapshot.
  void HandlePublishCatalog(const std::string& id,
                            const base::DictValue& route,
                            const base::DictValue& payload,
                            std::vector<blink::MessagePortDescriptor> ports);
  // nexus_web → C++ : pushes a SessionStatus snapshot.
  void HandlePublishStatus(const std::string& id,
                           const base::DictValue& route,
                           const base::DictValue& payload,
                           std::vector<blink::MessagePortDescriptor> ports);

  // Increments `model_call_in_flight_` and broadcasts the new
  // model-busy state if it just transitioned. Paired with
  // `OnModelCallFinished()` from each request's response callback.
  void OnModelCallStarted();
  void OnModelCallFinished();

  // Recomputes `modelBusy = (model_call_in_flight_ > 0) ||
  // !bridge_stream_states_.empty()` and, if that changed since the last
  // broadcast, fires `nexusAgentStateChanged` with `{ modelBusy }`. Call
  // after any change to either input.
  void BroadcastModelBusyIfChanged();

  raw_ptr<Profile> profile_;
  std::unique_ptr<NexusLlmHost> llm_host_;
  std::map<std::string, NexusLlmBridgeStreamState> bridge_stream_states_;
  // In-flight one-shot `model.call` requests (streams are tracked via
  // `bridge_stream_states_` instead). Used together with the stream map
  // to derive the model-busy phase shown in the topbar.
  int model_call_in_flight_ = 0;
  // Last value broadcast for `modelBusy`. We coalesce broadcasts so a
  // burst of stream chunks doesn't repeatedly re-send the same state.
  bool last_broadcast_model_busy_ = false;
  // Token returned by `LlmBroker::AddPickEventPusher`. Stored so we
  // can remove our pusher in the destructor without leaving a dangling
  // entry in the broker's fan-out map. The broker may have other
  // adapters' pushers too (page + worker bridge ports each register
  // one), so we cannot just clear the slot wholesale.
  std::optional<int> pick_event_pusher_token_;
  base::WeakPtrFactory<NexusLlmBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
