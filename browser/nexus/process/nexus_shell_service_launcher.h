// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PROCESS_NEXUS_SHELL_SERVICE_LAUNCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PROCESS_NEXUS_SHELL_SERVICE_LAUNCHER_H_

#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

// Browser-side helper that lazily launches the `NexusShellService`
// utility process via `content::ServiceProcessHost::Launch` and
// idle-recycles the remote after a period of inactivity.
//
// One launcher instance per `NexusBridgePortService` is sufficient — all
// guest sessions sharing that bridge funnel through the same remote, and
// the utility process runs everything off the binding sequence so
// concurrent calls do not block each other.
//
// IMPORTANT — UPSTREAM HOOK REQUIRED:
//   `ServiceProcessHost::Launch<nexus::mojom::NexusShellService>` returns
//   a non-functional remote until upstream Chromium's
//   `chrome/utility/services.cc` (function `RegisterIOThreadServices` or
//   equivalent for the relevant build target) registers the
//   `nexus::RunNexusShellService` factory from
//   `custom_browser/utility/nexus/shell/services.h`. Until that lands,
//   `GetService()` will return a remote whose first call disconnects
//   silently. See `docs/agents/nexus-process.md` §"Upstream-hook report"
//   for the exact diff.
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

  // Idle timeout used by the production launcher. Exposed for tests so
  // they can assert the value the LLM-facing docs reference.
  static constexpr base::TimeDelta kIdleTimeout = base::Seconds(30);

 private:
  void EnsureLaunched();
  void OnDisconnect();

  mojo::Remote<mojom::NexusShellService> remote_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PROCESS_NEXUS_SHELL_SERVICE_LAUNCHER_H_
