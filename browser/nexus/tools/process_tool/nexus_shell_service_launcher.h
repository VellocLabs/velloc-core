// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SHELL_SERVICE_LAUNCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SHELL_SERVICE_LAUNCHER_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_utility_service_remote.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;

namespace nexus {

// Lazily launches the `NexusShellService` utility process and idle-recycles
// the remote. Requires the upstream `chrome/utility/services.cc` hook that
// registers the service factory; without it the remote disconnects (P-7).
class NexusShellServiceLauncher {
 public:
  NexusShellServiceLauncher();

  NexusShellServiceLauncher(const NexusShellServiceLauncher&) = delete;
  NexusShellServiceLauncher& operator=(const NexusShellServiceLauncher&) =
      delete;

  ~NexusShellServiceLauncher();

  // Returns a connected remote, launching the utility process on first
  // use. Subsequent calls reuse the same remote until the idle timeout
  // elapses; once recycled, the next call relaunches.
  mojo::Remote<mojom::NexusShellService>& GetService();

  // Resets the underlying remote and tears down the utility process.
  // Called automatically by the idle handler; exposed for tests and
  // shutdown plumbing.
  void Shutdown();

  // Temporarily suppress idle-recycle while a long-lived child is running (the
  // persistent shells — NexusPersistentShells). Keepalive callers use opaque
  // ids as tokens; repeated retain/release is idempotent. Once the last token
  // is released, the normal 30s idle timer can recycle the utility process
  // again.
  void RetainBackgroundShell(const std::string& shell_id);
  void ReleaseBackgroundShell(const std::string& shell_id);

  bool IsBackgroundShellRetainedForTesting(const std::string& shell_id) const;
  size_t retained_background_shell_count_for_testing() const;

  base::WeakPtr<NexusShellServiceLauncher> GetWeakPtr();

  // Idle timeout used by the production launcher. Exposed for tests so
  // they can assert the value the LLM-facing docs reference.
  static constexpr base::TimeDelta kIdleTimeout = base::Seconds(30);

 private:
  void ConfigureIdleHandler();

  NexusUtilityServiceRemote<mojom::NexusShellService> remote_;
  std::set<std::string> retained_background_shells_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusShellServiceLauncher> weak_factory_{this};
};

// The per-Profile shared launcher used by ProcessTool, the `shell.*` adapter
// and NexusPersistentShells: one utility process. UI-thread only; 30s
// idle-recycle when no persistent child is retained.
NexusShellServiceLauncher& GetSharedShellLauncher(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SHELL_SERVICE_LAUNCHER_H_
