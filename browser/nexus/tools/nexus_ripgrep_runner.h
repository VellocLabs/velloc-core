// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_RIPGREP_RUNNER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_RIPGREP_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"

class Profile;

namespace nexus {

class NexusShellServiceLauncher;
struct EnsureBinaryResult;  // browser/nexus/binaries/nexus_binary_installer.h

// Outcome of a managed-ripgrep run, handed to the caller's continuation.
//
// Move-only and out-of-line per chromium-style: the std::string / vector
// members are non-trivial, so the clang plugin requires the special members
// be declared here and defined in the .cc.
struct RipgrepRunResult {
  RipgrepRunResult();
  RipgrepRunResult(RipgrepRunResult&&) noexcept;
  RipgrepRunResult& operator=(RipgrepRunResult&&) noexcept;
  RipgrepRunResult(const RipgrepRunResult&) = delete;
  RipgrepRunResult& operator=(const RipgrepRunResult&) = delete;
  ~RipgrepRunResult();

  // True when rg actually ran (ANY exit code). False for a pre-launch
  // failure (rg unavailable / cwd invalid / disallowed) — see error_*.
  bool launched = false;

  // Pre-launch failure detail (set only when `launched` is false).
  // `error_code` mirrors the shell service's code (e.g. BINARY_NOT_FOUND)
  // or "ripgrep_unavailable"; `error_message` is human-readable and already
  // carries an install hint when rg could not be auto-provisioned.
  std::string error_code;
  std::string error_message;

  // rg's process exit code (set only when `launched` is true). 0 = matches,
  // 1 = no matches (both success), >= 2 = a ripgrep error.
  int exit_code = 0;

  // rg stdout split into trimmed, CRLF-stripped, non-empty lines, capped to
  // the byte budget passed to Run(). Empty when exit_code == 1.
  std::vector<std::string> lines;
};

// Invariant exec inputs for a single Run(), bundled so they thread through
// the async install-retry without a long parameter list.
struct RipgrepRunOptions {
  // Absolute, existing directory. The shell service requires it; for rg it
  // does not drive matching when a target path is included in `rg_args`.
  base::FilePath cwd;
  uint32_t timeout_ms = 0;
  uint32_t max_output_bytes = 0;
};

// Runs the managed ripgrep through NexusShellService and returns its lines,
// asking NexusBinaryInstaller and retrying once on BINARY_NOT_FOUND. Used by
// Grep and Glob, on the caller's launcher; the owner must call Reset() on
// teardown OnStop. Safe for concurrent Run() calls.
class NexusRipgrepRunner {
 public:
  using ResultCallback = base::OnceCallback<void(RipgrepRunResult)>;

  NexusRipgrepRunner();
  ~NexusRipgrepRunner();

  NexusRipgrepRunner(const NexusRipgrepRunner&) = delete;
  NexusRipgrepRunner& operator=(const NexusRipgrepRunner&) = delete;

  // Exec `rg <rg_args>` (rg_args is argv WITHOUT argv[0]; include any target
  // path as the trailing entry). `profile` is needed only to fetch the URL
  // loader factory for the on-demand rg download; may be null (then a missing
  // rg cannot be auto-installed and surfaces as a pre-launch failure).
  // `callback` runs on the calling sequence with the outcome.
  void Run(NexusShellServiceLauncher* launcher,
           Profile* profile,
           std::vector<std::string> rg_args,
           RipgrepRunOptions options,
           ResultCallback callback);

  // Test-only parser entry point for exercising edge-case BigBuffer storage.
  static RipgrepRunResult ParseExecResultForTesting(
      ::nexus::mojom::ShellExecResultPtr result,
      size_t cap);

  // Invalidate pending callbacks (call from the owning tool's teardown).
  void Reset();

 private:
  void Launch(NexusShellServiceLauncher* launcher,
              Profile* profile,
              std::string command,
              std::vector<std::string> rg_args,
              RipgrepRunOptions options,
              bool allow_install_retry,
              ResultCallback callback);

  void OnExecDone(NexusShellServiceLauncher* launcher,
                  Profile* profile,
                  std::vector<std::string> rg_args,
                  RipgrepRunOptions options,
                  bool allow_install_retry,
                  ResultCallback callback,
                  bool ok,
                  ::nexus::mojom::ShellExecResultPtr result,
                  const std::optional<std::string>& error_code,
                  const std::optional<std::string>& error_message,
                  const std::optional<std::string>& install_hint);

  void OnRgEnsured(NexusShellServiceLauncher* launcher,
                   std::vector<std::string> rg_args,
                   RipgrepRunOptions options,
                   ResultCallback callback,
                   const EnsureBinaryResult& ensured);

  // UI-thread reply after the stdout parse hops back from the thread pool.
  // Bound through `weak_ptr_factory_` so a teardown mid-parse drops the reply
  // (and the captured `done`) cleanly instead of resolving into freed state.
  void OnParsed(ResultCallback callback, RipgrepRunResult result);

  base::WeakPtrFactory<NexusRipgrepRunner> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_RIPGREP_RUNNER_H_
