#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_EXEC_NEXUS_EXEC_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_EXEC_NEXUS_EXEC_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/exec/exec_api.gen.h"
#include "custom_browser/browser/nexus/api/shell/host_shell.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

namespace nexus {

class NexusBridgeResponseDelegate;
class ToolSessionRegistry;

// `exec` capability — the host actions behind the shell tools (bash,
// powerShell) plus `listShells`, which tells the runtime which of the two the
// host can run so it offers only those tools. Each tool action is a thin
// forward into the session's ProcessTool; the model-facing tool lives in the
// runtime. Declarations: api/declarations/exec/*.api.json.
class NexusExecBridgeAdapter : public ::nexus::generated::ExecApiBase {
 public:
  NexusExecBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                         ToolSessionRegistry* sessions);
  ~NexusExecBridgeAdapter() override;

  NexusExecBridgeAdapter(const NexusExecBridgeAdapter&) = delete;
  NexusExecBridgeAdapter& operator=(const NexusExecBridgeAdapter&) = delete;

 private:
  void HandleListShellsOnUI(
      ::nexus::generated::ExecListShellsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void ReplyListShells(::nexus::BridgeResponseCallback done,
                       HostShellAvailability availability);

  void HandleBashOnUI(
      ::nexus::generated::ExecBashInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandlePowerShellOnUI(
      ::nexus::generated::ExecPowerShellInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<ToolSessionRegistry> sessions_;
  base::WeakPtrFactory<NexusExecBridgeAdapter> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_EXEC_NEXUS_EXEC_BRIDGE_ADAPTER_H_
