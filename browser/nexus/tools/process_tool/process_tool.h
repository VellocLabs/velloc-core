#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_PROCESS_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_PROCESS_TOOL_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/tools/nexus_ripgrep_runner.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

// Generated category-aggregate base. One pure virtual per *.tool.json
// under declarations/process_tool/ — currently just `bash`. The header
// transitively pulls in each per-tool *.gen.h.
#include "custom_browser/browser/nexus/api/exec/exec_api.gen.h"
#include "custom_browser/browser/nexus/api/files/files_api.gen.h"

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
// `OnStop` invalidates the WeakPtrFactory so any pending mojo callbacks
// drop cleanly. The per-profile shared launcher is not shut down here;
// it self-recycles when idle.
class ProcessTool : public ToolBase {
 public:
  ProcessTool(ToolSession* session, Profile* profile,
              const std::string& session_key);
  ~ProcessTool() override;

  ProcessTool(const ProcessTool&) = delete;
  ProcessTool& operator=(const ProcessTool&) = delete;

  // Invalidates WeakPtrs and releases per-tool helpers. Idempotent.
  void OnStop(OnStopReason reason) override;

  // generated::ProcessToolBase overrides. Public because the codegen
  // dispatcher in nexus_tools_registry.gen.cc invokes through a
  // `ProcessTool*` it gets from `CategoryAccessor::Get(session)`.
  void RunBashOnUI(::nexus::generated::ExecBashInput input,
                   ToolCallback done);
  void RunGrepOnUI(::nexus::generated::FilesGrepInput input,
                   ToolCallback done);

  // Test-only entry points for the anonymous-namespace Grep helpers.
  // Exposed so process_tool_unittest.cc can pin spec 2-4-5 (arg build)
  // and 2-4-6 (result shaping) without a live utility process.
  static std::vector<std::string> BuildRipgrepArgsForTesting(
      const ::nexus::generated::FilesGrepInput& input,
      const std::string& output_mode,
      bool show_line_numbers);
  static std::vector<std::string> SplitGlobPatternsForTesting(
      const std::string& glob);
  // Applies the single-file Grep defaults (mode -> content, -C 5) exactly as
  // OnGrepPathChecked does before building argv; `target_is_file` is what
  // ClassifyGrepTargetOnBlocking would have reported.
  static void ApplySingleFileGrepDefaultsForTesting(
      ::nexus::generated::FilesGrepInput& input,
      std::string& output_mode,
      bool target_is_file);
  // Drive the exec / grep completion handlers with a service's pre-launch
  // failure, without a live service: the unit test pins that the rejected
  // code is the tool's own declared one and the service code is a param.
  void CompleteExecForTesting(ToolCallback done,
                              const std::string& tool_label,
                              const std::optional<std::string>& error_code,
                              const std::optional<std::string>& error_message,
                              const std::optional<std::string>& install_hint);
  void CompleteGrepForTesting(ToolCallback done, RipgrepRunResult result);
  void CompleteHookExecForTesting(
      ToolCallback done,
      const std::optional<std::string>& error_code,
      const std::optional<std::string>& error_message,
      const std::optional<std::string>& install_hint);
  // Windows-only PowerShell tool (the runtime offers it only when
  // `exec.listShells` reports a PowerShell). Tries PowerShell 7 (`pwsh`) first, falling
  // back to Windows PowerShell (`powershell`) when pwsh isn't installed, and
  // shapes the result through OnBashExecDone like the Bash tool's prose path.
  void RunPowerShellOnUI(::nexus::generated::ExecPowerShellInput input,
                         ToolCallback done);

 private:
  // Mojo callback for `NexusShellService::Exec` from `RunBashOnUI`.
  // Resolves `done` with the shaped Bash output dict, including the
  // command-semantic interpretation for commands like `grep` / `diff`
  // / `test` whose exit-1 means "no match" rather than "error".
  //
  // `tool_label` names the calling tool ("bash", "powershell", …) and is
  // used to build the rejection error code ("<label>_failed" /
  // "<label>_timeout"). It exists because `OnPowerShellExecDone` reuses
  // this prose shaper: without it a PowerShell failure would surface the
  // misleading `bash_failed` code.
  void OnBashExecDone(ToolCallback done,
                      std::string original_command,
                      base::TimeTicks start_time,
                      std::string tool_label,
                      bool ok,
                      ::nexus::mojom::ShellExecResultPtr result,
                      const std::optional<std::string>& error_code,
                      const std::optional<std::string>& error_message,
                      const std::optional<std::string>& install_hint);

  void OnForegroundSpawnDone(ToolCallback done,
                             std::string original_command,
                             base::TimeTicks start_time,
                             std::string tool_label,
                             bool ok,
                             const std::optional<std::string>& shell_id,
                             const std::optional<std::string>& error_code,
                             const std::optional<std::string>& error_message,
                             const std::optional<std::string>& install_hint);
  void PollForegroundShell(ToolCallback done,
                           std::string shell_id,
                           std::string original_command,
                           base::TimeTicks start_time,
                           std::string tool_label);
  void OnForegroundShellStatus(ToolCallback done,
                               std::string shell_id,
                               std::string original_command,
                               base::TimeTicks start_time,
                               std::string tool_label,
                               ::nexus::mojom::ShellBackgroundOutputPtr output);

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

  // Mojo callback for a foreground PowerShell attempt via
  // `NexusShellService::Spawn`. If `pwsh` wasn't found, retries once with
  // `powershell`; otherwise delegates to OnForegroundSpawnDone (progress
  // polling, then the prose shaping of OnBashExecDone).
  void OnPowerShellForegroundSpawnDone(ToolCallback done,
                                       std::string command,
                                       base::FilePath cwd,
                                       int timeout_ms,
                                       std::string attempted_shell,
                                       bool ok,
                                       const std::optional<std::string>& shell_id,
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

  // Continuation for `NexusRipgrepRunner::Run` from `RunGrepOnUI`. Shapes
  // the runner's parsed `lines` per `output_mode` (mirrors the TS `GrepTool`
  // result shape) and resolves `done`; on a pre-launch failure (rg
  // unavailable after the runner's auto-install attempt) rejects with the
  // runner-provided code + message.
  void OnGrepRgDone(ToolCallback done,
                    std::string output_mode,
                    int head_limit,
                    int offset,
                    base::FilePath cwd,
                    RipgrepRunResult result);

  // What the Grep target turned out to be on disk. `kUnknown` is the UNC
  // case: existence is assumed (no stat, to avoid NTLM credential leaks) so
  // the type cannot be known and no single-file default applies.
  enum class GrepTargetKind { kMissing, kFile, kDirectory, kUnknown };

  // Blocking half of the Grep target pre-check. Runs on a `base::ThreadPool`
  // worker (MayBlock) — NEVER on the UI thread, where `base::GetFileInfo`
  // would trip the blocking-disallowed DCHECK. Classifies `target` as
  // missing / regular file / directory.
  static GrepTargetKind ClassifyGrepTargetOnBlocking(base::FilePath target);

  // UI-thread reply to `ClassifyGrepTargetOnBlocking`. Rejects `done` with
  // `path_not_found` when the target is missing; applies the single-file
  // defaults when it is a regular file; then launches ripgrep through
  // `ripgrep_runner_` (exec + auto-install + line shaping).
  void OnGrepPathChecked(::nexus::generated::FilesGrepInput input,
                         ToolCallback done,
                         std::string output_mode,
                         bool show_line_numbers,
                         int head_limit,
                         int offset,
                         base::FilePath target,
                         base::FilePath default_root,
                         RipgrepRunOptions options,
                         GrepTargetKind target_kind);

  // Blocking half of the Grep `files_with_matches` result shaping. Runs on a
  // `base::ThreadPool` worker (MayBlock) — NEVER on the UI thread — because it
  // stats every matched file (`base::GetFileInfo`) to sort by mtime descending
  // (spec 2-4). Returns the fully-shaped result body (or "No files found").
  static base::DictValue ShapeGrepFilesWithMatchesBlocking(
      std::vector<std::string> lines,
      int head_limit,
      int offset,
      base::FilePath cwd);

  // UI-thread reply to `ShapeGrepFilesWithMatchesBlocking`. Resolves `done`
  // with the shaped body.
  void OnGrepFilesShaped(ToolCallback done, base::DictValue body);

  // Per-PROFILE shared launcher (Profile user-data via
  // `GetSharedShellLauncher`), NOT owned — one utility process for the
  // profile's shell tools and the guest `shell.*` capability. `OnStop` does
  // NOT shut it down (other sessions of the
  // profile share it; it idle-recycles itself). Shared by Bash / PowerShell /
  // Grep.
  raw_ptr<NexusShellServiceLauncher> launcher_;

  // Shared rg orchestrator backing the Grep tool (exec + auto-install +
  // line parsing). Borrows `launcher_`; `OnStop` resets it so in-flight
  // mojo / installer callbacks drop cleanly on teardown.
  NexusRipgrepRunner ripgrep_runner_;

  base::WeakPtrFactory<ProcessTool> weak_ptr_factory_{this};
};

}  // namespace nexus


#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_PROCESS_TOOL_H_
