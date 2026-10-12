#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FILES_NEXUS_FILES_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FILES_NEXUS_FILES_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/files/files_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

namespace nexus {

class NexusBridgeResponseDelegate;
class ToolSessionRegistry;

// `files` capability — the host actions behind the file tools (read, write,
// edit, glob, grep), each a thin forward into the session's FileTool /
// ProcessTool. The handler runs on UI and only resolves the session; tool
// bodies run in arrival order on ToolSession::blocking_runner.
class NexusFilesBridgeAdapter : public ::nexus::generated::FilesApiBase {
 public:
  NexusFilesBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                          ToolSessionRegistry* sessions);
  ~NexusFilesBridgeAdapter() override;

  NexusFilesBridgeAdapter(const NexusFilesBridgeAdapter&) = delete;
  NexusFilesBridgeAdapter& operator=(const NexusFilesBridgeAdapter&) = delete;

 private:
  void HandleEditOnUI(
      ::nexus::generated::FilesEditInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleGlobOnUI(
      ::nexus::generated::FilesGlobInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleReadOnUI(
      ::nexus::generated::FilesReadInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleWriteOnUI(
      ::nexus::generated::FilesWriteInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleGrepOnUI(
      ::nexus::generated::FilesGrepInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<ToolSessionRegistry> sessions_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FILES_NEXUS_FILES_BRIDGE_ADAPTER_H_
