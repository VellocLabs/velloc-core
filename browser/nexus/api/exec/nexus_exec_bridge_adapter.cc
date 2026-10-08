#include "custom_browser/browser/nexus/api/exec/nexus_exec_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/tools/process_tool/process_tool.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/tools/tool_session_registry.h"

namespace nexus {

NexusExecBridgeAdapter::NexusExecBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    ToolSessionRegistry* sessions)
    : ::nexus::generated::ExecApiBase(delegate), sessions_(sessions) {}

NexusExecBridgeAdapter::~NexusExecBridgeAdapter() = default;

void NexusExecBridgeAdapter::HandleListShellsOnUI(
    ::nexus::generated::ExecListShellsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Probing PATH (and the Git for Windows locations) touches the disk.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ResolveHostShellAvailability),
      base::BindOnce(&NexusExecBridgeAdapter::ReplyListShells,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done)));
}

void NexusExecBridgeAdapter::ReplyListShells(
    ::nexus::BridgeResponseCallback done,
    HostShellAvailability availability) {
  base::DictValue result;
#if BUILDFLAG(IS_WIN)
  result.Set("bash", availability.bash);
  result.Set("powershell", availability.powershell());
#else
  // Off Windows a POSIX shell is always there and PowerShell is never
  // offered.
  result.Set("bash", true);
  result.Set("powershell", false);
#endif
  std::move(done).Success(std::move(result));
}

void NexusExecBridgeAdapter::HandleBashOnUI(
    ::nexus::generated::ExecBashInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "exec", "bash", "Bash", std::move(done),
      base::BindOnce(
          [](::nexus::generated::ExecBashInput input, ToolSession& session,
             ToolCallback callback) {
            session.process_tool().RunBashOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusExecBridgeAdapter::HandlePowerShellOnUI(
    ::nexus::generated::ExecPowerShellInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "exec", "powerShell", "PowerShell", std::move(done),
      base::BindOnce(
          [](::nexus::generated::ExecPowerShellInput input, ToolSession& session,
             ToolCallback callback) {
            session.process_tool().RunPowerShellOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

}  // namespace nexus
