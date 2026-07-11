#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_PROCESS_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_PROCESS_TOOL_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_macros.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

// Generated category-aggregate base. One pure virtual per *.tool.json
// under declarations/process_tool/ — currently just `bash`. The header
// transitively pulls in each per-tool *.gen.h.
#include "custom_browser/browser/nexus/tools/process_tool.gen.h"

class Profile;

namespace nexus {

class NexusShellServiceLauncher;
class ToolSession;  // for CategoryAccessor specialization below

// Per-LLM-session class hosting tools that shell out to the host
// machine via the sandboxed `NexusShellService` utility process.
// Today it owns two tools: `Bash` (arbitrary shell command) and
// `Grep` (content search via ripgrep). Both fire async mojo calls
// from UI and resolve from the mojo callback. Future shell-bound
// tools (streaming `Spawn`, native `which`, …) land here.
//
// Threading: the codegen-emitted dispatcher invokes `RunBashOnUI`
// directly on the calling sequence (UI today — the bridge port
// service runs there) because `bash.tool.json` declares
// `runtime.thread = "ui"`. The body fires the async mojo
// `Exec(options, callback)` on the UI thread and resolves `done` from
// the mojo callback (also UI). No worker-thread hopping required.
//
// The mojo `Remote<NexusShellService>` is owned by a per-session
// `NexusShellServiceLauncher`. The launcher lazily binds the utility
// process on first `GetService()`; the utility process exits after
// 30s of inactivity and is re-launched on demand. Per-session
// launchers are wasteful compared to per-port-service launchers (one
// utility process per session vs. one shared) but the idle-recycle
// makes the difference small in practice and avoids plumbing the
// shared launcher through `ToolsAdapter` -> `ToolSession`.
//
// `OnStop` tears down the launcher (releases any in-flight Remote)
// and invalidates the WeakPtrFactory so any pending mojo callbacks
// drop cleanly.
class ProcessTool : public ToolBase,
                    public ::nexus::generated::ProcessToolBase {
 public:
  ProcessTool(ToolSession* session, Profile* profile,
              const std::string& session_key);
  ~ProcessTool() override;

  ProcessTool(const ProcessTool&) = delete;
  ProcessTool& operator=(const ProcessTool&) = delete;

  // No-op for symmetry with BrowserTool / FileTool. All registration
  // flows through the codegen-emitted generated::PopulateRegistry.
  static void DefineTools(ToolRegistry& registry);

  // Tears down the per-session launcher (releases the utility
  // process) and invalidates WeakPtrs. Idempotent.
  void OnStop(OnStopReason reason) override;

  // generated::ProcessToolBase overrides. Public because the codegen
  // dispatcher in nexus_tools_registry.gen.cc invokes through a
  // `ProcessTool*` it gets from `CategoryAccessor::Get(session)`.
  void RunBashOnUI(::nexus::generated::BashInput input,
                   ToolCallback done) override;
  void RunGrepOnUI(::nexus::generated::GrepInput input,
                   ToolCallback done) override;
  // Windows-only model-facing PowerShell tool (gated into the catalog by
  // the listHostedTools filter). Tries PowerShell 7 (`pwsh`) first, falling
  // back to Windows PowerShell (`powershell`) when pwsh isn't installed, and
  // shapes the result through OnBashExecDone like the Bash tool's prose path.
  void RunPowerShellOnUI(::nexus::generated::PowerShellInput input,
                         ToolCallback done) override;

 private:
  // Mojo callback for `NexusShellService::Exec` from `RunBashOnUI`.
  // Resolves `done` with the shaped Bash output dict, including the
  // command-semantic interpretation for commands like `grep` / `diff`
  // / `test` whose exit-1 means "no match" rather than "error".
  void OnBashExecDone(ToolCallback done,
                      std::string original_command,
                      base::TimeTicks start_time,
                      bool ok,
                      ::nexus::mojom::ShellExecResultPtr result,
                      const std::optional<std::string>& error_code,
                      const std::optional<std::string>& error_message,
                      const std::optional<std::string>& install_hint);

  // Launch one PowerShell attempt with `shell_id` ("pwsh" or "powershell").
  // Shared by RunPowerShellOnUI and its pwsh→powershell fallback retry.
  void LaunchPowerShellAttempt(std::string command,
                               base::FilePath cwd,
                               int timeout_ms,
                               std::string shell_id,
                               ToolCallback done);

  // Mojo callback for a PowerShell attempt. If `pwsh` wasn't found, retries
  // once with `powershell` (Windows PowerShell, universally present);
  // otherwise delegates to OnBashExecDone for identical prose shaping.
  void OnPowerShellExecDone(ToolCallback done,
                            std::string command,
                            base::FilePath cwd,
                            int timeout_ms,
                            std::string attempted_shell,
                            base::TimeTicks start_time,
                            bool ok,
                            ::nexus::mojom::ShellExecResultPtr result,
                            const std::optional<std::string>& error_code,
                            const std::optional<std::string>& error_message,
                            const std::optional<std::string>& install_hint);

  // Mojo callback for `NexusShellService::Exec` from the hook-exec branch
  // of `RunBashOnUI` (the internal `hook_shell` path). Resolves `done` with
  // a raw `{output, exit_code}` dict — no prose shaping, no reject on
  // non-zero exit — so the JS hook executor can apply its own exit-code
  // protocol. Rejects only on pre-launch failure.
  void OnHookExecDone(ToolCallback done,
                      bool ok,
                      ::nexus::mojom::ShellExecResultPtr result,
                      const std::optional<std::string>& error_code,
                      const std::optional<std::string>& error_message,
                      const std::optional<std::string>& install_hint);

  // Mojo callback for `NexusShellService::Exec` from `RunGrepOnUI`.
  // Parses ripgrep's stdout per `output_mode`, applies head_limit /
  // offset, and resolves `done` with the shaped Grep output dict
  // (mirrors the TS `GrepTool` result shape).
  void OnGrepExecDone(ToolCallback done,
                      std::string output_mode,
                      int head_limit,
                      int offset,
                      bool ok,
                      ::nexus::mojom::ShellExecResultPtr result,
                      const std::optional<std::string>& error_code,
                      const std::optional<std::string>& error_message,
                      const std::optional<std::string>& install_hint);

  // Per-session lazily-bound launcher. Created in the ctor so the
  // session owns its own utility process. `OnStop` resets it to drop
  // any in-flight remote.
  std::unique_ptr<NexusShellServiceLauncher> launcher_;

  base::WeakPtrFactory<ProcessTool> weak_ptr_factory_{this};
};

}  // namespace nexus

// Declare the macro DSL's category accessor for `ProcessTool`. The
// matching `NEXUS_DEFINE_CATEGORY(ProcessTool, process_tool)` lives in
// `process_tool.cc`, which can include `tool_session.h` (the header
// can't — `tool_session.h` includes this file to embed `ProcessTool`
// as a member).
NEXUS_DECLARE_CATEGORY(nexus::ProcessTool);

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_PROCESS_TOOL_H_
