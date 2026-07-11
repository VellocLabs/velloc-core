#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

  std::unique_ptr<NexusLlmHost> llm_host_;
  std::map<std::string, NexusLlmBridgeStreamState> bridge_stream_states_;
  base::WeakPtrFactory<NexusLlmBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_NEXUS_LLM_BRIDGE_ADAPTER_H_
