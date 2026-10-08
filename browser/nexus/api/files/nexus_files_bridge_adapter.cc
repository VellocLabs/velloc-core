#include "custom_browser/browser/nexus/api/files/nexus_files_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "custom_browser/browser/nexus/tools/file_tool/file_tool.h"
#include "custom_browser/browser/nexus/tools/process_tool/process_tool.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/tools/tool_session_registry.h"

namespace nexus {

NexusFilesBridgeAdapter::NexusFilesBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    ToolSessionRegistry* sessions)
    : ::nexus::generated::FilesApiBase(delegate), sessions_(sessions) {}

NexusFilesBridgeAdapter::~NexusFilesBridgeAdapter() = default;

void NexusFilesBridgeAdapter::HandleEditOnUI(
    ::nexus::generated::FilesEditInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "files", "edit", "Edit", std::move(done),
      base::BindOnce(
          [](::nexus::generated::FilesEditInput input, ToolSession& session,
             ToolCallback callback) {
            session.file_tool().RunEditOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusFilesBridgeAdapter::HandleGlobOnUI(
    ::nexus::generated::FilesGlobInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "files", "glob", "Glob", std::move(done),
      base::BindOnce(
          [](::nexus::generated::FilesGlobInput input, ToolSession& session,
             ToolCallback callback) {
            session.file_tool().RunGlobOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusFilesBridgeAdapter::HandleGrepOnUI(
    ::nexus::generated::FilesGrepInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "files", "grep", "Grep", std::move(done),
      base::BindOnce(
          [](::nexus::generated::FilesGrepInput input, ToolSession& session,
             ToolCallback callback) {
            session.process_tool().RunGrepOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusFilesBridgeAdapter::HandleReadOnUI(
    ::nexus::generated::FilesReadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "files", "read", "Read", std::move(done),
      base::BindOnce(
          [](::nexus::generated::FilesReadInput input, ToolSession& session,
             ToolCallback callback) {
            session.file_tool().RunReadOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusFilesBridgeAdapter::HandleWriteOnUI(
    ::nexus::generated::FilesWriteInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "files", "write", "Write", std::move(done),
      base::BindOnce(
          [](::nexus::generated::FilesWriteInput input, ToolSession& session,
             ToolCallback callback) {
            session.file_tool().RunWriteOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

}  // namespace nexus
