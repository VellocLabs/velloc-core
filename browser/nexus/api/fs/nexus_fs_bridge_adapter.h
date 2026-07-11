#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_BRIDGE_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "custom_browser/browser/nexus/api/fs/fs_api.gen.h"
#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `FsApiBase` virtuals.
//
// Phase 10g architecture — the adapter is now a pure business-logic
// layer:
//   - Per *.api.json declares its inputs in tools-style `inputs[]`,
//     including a `bytes` type that supports DUAL TRANSPORT (inline
//     array<integer> in the payload OR a transferred MessagePort
//     delivering an ArrayBuffer).
//   - The codegen-emitted dispatcher handles all of:
//       (a) typed input extraction
//       (b) inline-vs-port resolution for `bytes` inputs (calls the
//           framework's NexusBridgeCapabilityHandler::AwaitBinaryFromPort
//           when bytes need to come from a port)
//       (c) PostTasking the entry hop to the declared thread
//       (d) constructing the BridgeResponseCallback for the response
//   - Each Handle*OnThreadPool runs on base::ThreadPool, sees a
//     fully-populated typed input, fires the response via
//     std::move(done).Success/Error/Binary. Zero PostTasks in this file.
class NexusFsBridgeAdapter : public ::nexus::generated::FsApiBase {
 public:
  NexusFsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                       const base::FilePath& cwd);
  ~NexusFsBridgeAdapter() override;

  NexusFsBridgeAdapter(const NexusFsBridgeAdapter&) = delete;
  NexusFsBridgeAdapter& operator=(const NexusFsBridgeAdapter&) = delete;

 private:
  // Overrides of generated::FsApiBase pure virtuals. All run on
  // base::ThreadPool with MayBlock() — the codegen dispatcher PostTasks
  // each entry hop. Bodies do their blocking I/O via fs_host_ and fire
  // `done` for the response. The framework handles everything else.

  void HandleCwdOnThreadPool(
      ::nexus::generated::CwdInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStatOnThreadPool(
      ::nexus::generated::StatInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReaddirOnThreadPool(
      ::nexus::generated::ReaddirInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadFileOnThreadPool(
      ::nexus::generated::ReadFileInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadFileBytesOnThreadPool(
      ::nexus::generated::ReadFileBytesInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadRangeOnThreadPool(
      ::nexus::generated::ReadRangeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleWriteFileOnThreadPool(
      ::nexus::generated::WriteFileInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleAppendFileOnThreadPool(
      ::nexus::generated::AppendFileInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleMkdirOnThreadPool(
      ::nexus::generated::MkdirInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRmOnThreadPool(
      ::nexus::generated::RmInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleUnlinkOnThreadPool(
      ::nexus::generated::UnlinkInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleExistsOnThreadPool(
      ::nexus::generated::ExistsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  std::unique_ptr<NexusFsHost> fs_host_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_BRIDGE_ADAPTER_H_
