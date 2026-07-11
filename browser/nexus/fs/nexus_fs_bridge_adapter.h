#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_FS_NEXUS_FS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_FS_NEXUS_FS_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusFsHost;
class NexusMessagePortEndpoint;

class NexusFsBridgeAdapter : public NexusBridgeCapabilityHandler {
 public:
  NexusFsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                       const base::FilePath& cwd);
  ~NexusFsBridgeAdapter() override;

  NexusFsBridgeAdapter(const NexusFsBridgeAdapter&) = delete;
  NexusFsBridgeAdapter& operator=(const NexusFsBridgeAdapter&) = delete;

 private:
  void RemovePendingBinaryRequestPort(const std::string& request_id);
  void RemovePendingBinaryRequestPortSoon(const std::string& request_id);

  void HandleCwd(const std::string& id,
                 const base::DictValue& route,
                 const base::DictValue& payload,
                 std::vector<blink::MessagePortDescriptor> ports);
  void HandleStat(const std::string& id,
                  const base::DictValue& route,
                  std::string path,
                  std::vector<blink::MessagePortDescriptor> ports);
  void HandleReaddir(const std::string& id,
                     const base::DictValue& route,
                     std::string path,
                     std::vector<blink::MessagePortDescriptor> ports);
  void HandleReadFile(const std::string& id,
                      const base::DictValue& route,
                      std::string path,
                      std::vector<blink::MessagePortDescriptor> ports);
  void HandleReadFileBytes(const std::string& id,
                           const base::DictValue& route,
                           std::string path,
                           const base::DictValue& payload,
                           std::vector<blink::MessagePortDescriptor> ports);
  void HandleReadRange(const std::string& id,
                       const base::DictValue& route,
                       std::string path,
                       const base::DictValue& payload,
                       std::vector<blink::MessagePortDescriptor> ports);
  void HandleWriteFile(const std::string& id,
                       const base::DictValue& route,
                       std::string path,
                       const base::DictValue& payload,
                       std::vector<blink::MessagePortDescriptor> ports);
  void HandleAppendFile(const std::string& id,
                        const base::DictValue& route,
                        std::string path,
                        const base::DictValue& payload,
                        std::vector<blink::MessagePortDescriptor> ports);
  void HandleMkdir(const std::string& id,
                   const base::DictValue& route,
                   std::string path,
                   const base::DictValue& payload,
                   std::vector<blink::MessagePortDescriptor> ports);
  void HandleRm(const std::string& id,
                const base::DictValue& route,
                std::string path,
                const base::DictValue& payload,
                std::vector<blink::MessagePortDescriptor> ports);
  void HandleUnlink(const std::string& id,
                    const base::DictValue& route,
                    std::string path,
                    std::vector<blink::MessagePortDescriptor> ports);
  void HandleExists(const std::string& id,
                    const base::DictValue& route,
                    std::string path,
                    std::vector<blink::MessagePortDescriptor> ports);

  std::unique_ptr<NexusFsHost> fs_host_;
  std::map<std::string, std::unique_ptr<NexusMessagePortEndpoint>>
      pending_binary_request_ports_;
  base::WeakPtrFactory<NexusFsBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_FS_NEXUS_FS_BRIDGE_ADAPTER_H_
