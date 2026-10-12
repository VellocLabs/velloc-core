// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SAFE_COMMAND_POLICY_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SAFE_COMMAND_POLICY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/process/launch.h"
#include "custom_browser/utility/nexus/shell/shell_resolver.h"

namespace nexus {

// Hard server-side caps and defaults for `NexusShellService::Exec`. Kept
// in the header so unit tests can assert against the same constants the
// production path uses.
struct SafeCommandLimits {
  static constexpr uint32_t kDefaultTimeoutMs = 30u * 1000u;
  // Hard server-side ceiling on a single command's wall-clock. Kept in
  // sync with the browser-side `kMaxBashTimeoutMs` (process_tool.cc =
  // 600000) so the utility never silently kills a command earlier than
  // the Bash tool advertises to the model — previously this was 5 min
  // while the tool accepted `timeout` up to 10 min, so a command set to
  // the advertised max was killed at the half-way point. Same 10-minute
  // ceiling as the Bash tool's `timeout` schema (spec 2-6).
  static constexpr uint32_t kMaxTimeoutMs = 10u * 60u * 1000u;

  // No-timeout-cap sentinel. When a command's requested timeout equals this,
  // ClampTimeoutMs passes it through UNCLAMPED (rather than lowering to
  // kMaxTimeoutMs) so the process runs with no meaningful wall-clock kill —
  // `base::Milliseconds(kNoTimeoutCapSentinelMs)` is ~24.8 days, effectively
  // unbounded; the child still ends on natural exit, an explicit Kill, or the
  // output-size watchdog. Set only by the auto-background middle layer (a
  // detached foreground command that must run to completion, matching Claude
  // Code's backgrounded-command lifetime — `background()` clears the kill timer).
  // Chosen as INT32_MAX: it fits the browser-side `int timeout_ms`, and an OLD
  // browser that predates the sentinel clamps it to kMaxTimeoutMs (the prior
  // 10-minute kill), so version skew degrades safely. No browser-side caller
  // sends it: the hosted Bash / PowerShell tools no longer
  // background (the runtime owns background shells over shell.spawn+persist,
  // which has no wall-clock cap). Retained as a policy passthrough.
  static constexpr uint32_t kNoTimeoutCapSentinelMs = 2147483647u;

  static constexpr uint32_t kDefaultMaxOutputBytes = 1u * 1024u * 1024u;
  static constexpr uint32_t kMaxMaxOutputBytes = 16u * 1024u * 1024u;
};

// One env override coming from the guest. Mirrors
// `nexus.mojom.ShellEnvEntry` but lives in pure C++ so the policy can be
// tested without going through Mojo.
struct EnvOverride {
  std::string name;
  std::string value;
};

// Outcome of a pre-launch policy check. Exactly one of `error_code` and
// `resolved_path` is non-empty: success populates `resolved_path` and
// leaves `error_code` empty; rejection sets `error_code` (and a
// human-readable `error_message`) and clears `resolved_path`.
//
// When `error_code == "BINARY_NOT_FOUND"`, `install_hint` is populated
// with a multi-line, model-facing instruction string describing how to
// install the missing binary on this host. For other error codes
// `install_hint` is empty.
struct PolicyDecision {
  PolicyDecision();
  ~PolicyDecision();
  PolicyDecision(const PolicyDecision&);
  PolicyDecision& operator=(const PolicyDecision&);
  PolicyDecision(PolicyDecision&&);
  PolicyDecision& operator=(PolicyDecision&&);

  base::FilePath resolved_path;       // absolute path to argv[0]
  std::string error_code;             // e.g. "COMMAND_NOT_ALLOWED"
  std::string error_message;          // safe to surface to the LLM
  std::string install_hint;           // BINARY_NOT_FOUND only; possibly empty

  bool ok() const { return error_code.empty(); }
};

// Pure-C++ checks shared by `NexusShellServiceImpl::Exec` and unit
// tests. None of these touch the filesystem except `ValidateCwd`, which
// only stats `cwd` itself (no traversal).
class SafeCommandPolicy {
 public:
  // Resolves `command` to an absolute executable path using the
  // already-probed capability table. Accepts:
  //   * a known tool id present in `caps.tools` with `available == true`
  //     AND listed in `caps.command_allowlist`;
  //   * an absolute path that case-insensitively (Windows) /
  //     case-sensitively (POSIX) matches one of the resolved tool paths.
  // Anything else is rejected with COMMAND_NOT_ALLOWED.
  static PolicyDecision ResolveCommand(const std::string& command,
                                       const ResolvedCapabilities& caps);

  // Returns true and fills `out_reason` empty when `cwd` is absolute,
  // exists, and is a directory. Returns false otherwise with a
  // human-readable reason.
  static bool ValidateCwd(const base::FilePath& cwd, std::string* out_reason);

  // Builds the full env map that the child will see. Starts from the
  // process-baseline whitelist (PATH, HOME / USERPROFILE, SystemRoot,
  // etc. — see implementation for the platform-specific lists) read
  // from the utility process's own env, then layers `extras` on top.
  // Forbidden names (LD_PRELOAD, DYLD_*, NODE_OPTIONS, ...) are dropped
  // from `extras` and never inherited from the baseline.
  //
  // The returned map is suitable for `base::LaunchOptions::environment`
  // when paired with `clear_environment = true`.
  static base::EnvironmentMap BuildSafeEnvironment(
      const std::vector<EnvOverride>& extras);

  // Mutates `env` for a resolved command after policy resolution. Currently
  // Windows-only: when `command_id == "bash"` resolves to Git for Windows,
  // prepends the adjacent Git dirs (usr/bin, mingw64/bin, bin, cmd) so commands
  // launched inside Git Bash can find the MSYS/Git toolchain even if the
  // browser's global PATH did not include it. No-op for all other commands.
  static void AugmentEnvironmentForResolvedCommand(
      const std::string& command_id,
      const base::FilePath& resolved_path,
      base::EnvironmentMap* env);

  // Names that are *always* dropped, whether they came from the
  // process baseline or from the guest `extras`. Exposed for tests.
  static const std::vector<std::string>& ForbiddenEnvNames();

  // Names always inherited from the parent process when present.
  // Exposed for tests.
  static const std::vector<std::string>& InheritedEnvNames();

  // Clamp helpers: zero means "use default"; values above the cap are
  // silently lowered. Exposed for tests so the guarantees are explicit.
  static uint32_t ClampTimeoutMs(uint32_t requested);
  static uint32_t ClampMaxOutputBytes(uint32_t requested);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SAFE_COMMAND_POLICY_H_
