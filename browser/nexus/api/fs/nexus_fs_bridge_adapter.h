#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_BRIDGE_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
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
//       (c) invoking the Handle*OnUI entry point synchronously on UI
//       (d) constructing the BridgeResponseCallback for the response
//   - Each Handle*OnUI runs on UI, sees a fully-populated typed input,
//     and hops the blocking fs work to base::ThreadPool via
//     PostTaskAndReplyWithResult. The worker task holds a
//     scoped_refptr<NexusFsHost> (NOT `this`), so a port-service
//     teardown racing an in-flight op cannot use-after-free the adapter;
//     the reply delivers via std::move(done).Success/Error/Binary back
//     on UI (weak-guarded by the BridgeResponseCallback). See
//     docs/agents/nexus-tools-api.md §6.
class NexusFsBridgeAdapter : public ::nexus::generated::FsApiBase {
 public:
  NexusFsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                       const base::FilePath& cwd);
  ~NexusFsBridgeAdapter() override;

  NexusFsBridgeAdapter(const NexusFsBridgeAdapter&) = delete;
  NexusFsBridgeAdapter& operator=(const NexusFsBridgeAdapter&) = delete;

 private:
  // Overrides of generated::FsApiBase pure virtuals. The codegen
  // dispatcher invokes each on UI (the fs *.api.json declare
  // runtime.thread = "ui"). Each body hops the blocking I/O to
  // base::ThreadPool via PostTaskAndReplyWithResult, binding the worker
  // task to a scoped_refptr<NexusFsHost> and replying back on UI.

  void HandleCwdOnUI(
      ::nexus::generated::CwdInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleUserHomeOnUI(
      ::nexus::generated::UserHomeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStatOnUI(
      ::nexus::generated::StatInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReaddirOnUI(
      ::nexus::generated::ReaddirInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStatBatchOnUI(
      ::nexus::generated::StatBatchInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReaddirTreeOnUI(
      ::nexus::generated::ReaddirTreeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadBatchOnUI(
      ::nexus::generated::ReadBatchInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadFileOnUI(
      ::nexus::generated::ReadFileInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadFileBytesOnUI(
      ::nexus::generated::ReadFileBytesInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleReadRangeOnUI(
      ::nexus::generated::ReadRangeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleWriteFileOnUI(
      ::nexus::generated::WriteFileInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleAppendFileOnUI(
      ::nexus::generated::AppendFileInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleMkdirOnUI(
      ::nexus::generated::MkdirInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRmOnUI(
      ::nexus::generated::RmInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleUnlinkOnUI(
      ::nexus::generated::UnlinkInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleExistsOnUI(
      ::nexus::generated::ExistsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRealpathOnUI(
      ::nexus::generated::RealpathInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSystemInfoOnUI(
      ::nexus::generated::SystemInfoInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // RefCounted so an in-flight thread-pool task (which binds a
  // scoped_refptr copy) keeps the host alive even if this adapter is
  // destroyed mid-op by port-service teardown.
  scoped_refptr<NexusFsHost> fs_host_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_BRIDGE_ADAPTER_H_
