#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_HOST_SHELL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_HOST_SHELL_H_

#include <optional>
#include <string>

namespace nexus {

// Which shells this host can run. The one resolver behind both the
// listHostedTools gate (whether the Bash / PowerShell tools are offered) and
// the `fs.systemInfo` shell name (the guest prompt's `Shell:` line), so the
// shell the prompt names is always the shell the offered tool runs.
struct HostShellAvailability {
  // `bash` on PATH, or (Windows) a Git for Windows install — what the Bash
  // tool invokes (process_tool.cc BuildShellInvocation).
  bool bash = false;
  // `pwsh` (PowerShell 7+) on PATH — the PowerShell tool's first choice.
  bool pwsh_core = false;
  // Windows PowerShell (`powershell`) on PATH — the PowerShell tool's
  // fallback when `pwsh` is missing.
  bool windows_powershell = false;

  bool powershell() const { return pwsh_core || windows_powershell; }
};

// Resolves HostShellAvailability from PATH and, on Windows, the usual Git for
// Windows locations. Stats files: MUST run on a MayBlock sequence.
HostShellAvailability ResolveHostShellAvailability();

// The interpreter name the prompt's `Shell:` line should carry, path and
// `.exe` stripped ("bash", "zsh", "pwsh"). Empty means unknown; the guest then
// drops the line.
//
// Windows: the shell the model's shell tool actually runs — `bash` whenever
// the Bash tool is offered, else the PowerShell the PowerShell tool launches,
// else `%COMSPEC%` as a last resort. `$SHELL` / `%COMSPEC%` alone are not
// used while a tool shell exists: an app started from Explorer has no
// `$SHELL`, and reporting `cmd` there told the model to write cmd syntax
// (`set VAR=value && …`) into commands Git Bash runs.
//
// POSIX: `$SHELL`, the interpreter the user chose.
//
// Pure; the env values are passed in so it is testable.
std::string ChooseReportedShellName(const HostShellAvailability& shells,
                                    std::optional<std::string> shell_env,
                                    std::optional<std::string> comspec_env);

// ChooseReportedShellName over the live host: resolves the availability and
// reads `$SHELL` / `%COMSPEC%`. Stats files: MUST run on a MayBlock sequence.
std::string ResolveReportedShellName();

// Reduces a shell path to its interpreter name — `/usr/bin/zsh` → `zsh`,
// `C:\Program Files\Git\bin\bash.exe` → `bash` (`.exe` stripped on Windows).
// Unset or empty input → empty.
std::string ExtractShellBasename(std::optional<std::string> raw);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_HOST_SHELL_H_
