// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SHELL_SERVICE_LAUNCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SHELL_SERVICE_LAUNCHER_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_utility_service_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;

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
class NexusShellServiceLauncher : public mojom::NexusBackgroundShellObserver {
 public:
  // Reply shape for `SpawnBackgroundWithObserver` — mirrors the mojo
  // `Spawn`/`SpawnWithObserver` response so callers (ProcessTool) can reuse
  // their existing spawn-reply handling verbatim.
  using SpawnReplyCallback = base::OnceCallback<void(
      bool ok,
      const std::optional<std::string>& shell_id,
      const std::optional<std::string>& error_code,
      const std::optional<std::string>& error_message,
      const std::optional<std::string>& install_hint)>;

  // Notified when ANY background shell reaches a terminal state. Each
  // connected bridge guest registers one sink (its `SendEvent`) so the launcher
  // can broadcast `shell.onExit` to every guest; each guest filters by shellId
  // on the JS side. Repeating because many shells exit over the launcher's
  // lifetime.
  using ShellEventSink =
      base::RepeatingCallback<void(const std::string& shell_id,
                                   const mojom::ShellExitInfo&)>;

  NexusShellServiceLauncher();

  NexusShellServiceLauncher(const NexusShellServiceLauncher&) = delete;
  NexusShellServiceLauncher& operator=(const NexusShellServiceLauncher&) =
      delete;

  ~NexusShellServiceLauncher() override;

  // mojom::NexusBackgroundShellObserver:
  void OnExit(const std::string& shell_id,
              mojom::ShellExitInfoPtr exit) override;

  // Returns a connected remote, launching the utility process on first
  // use. Subsequent calls reuse the same remote until the idle timeout
  // elapses; once recycled, the next call relaunches.
  mojo::Remote<mojom::NexusShellService>& GetService();

  // Resets the underlying remote and tears down the utility process.
  // Called automatically by the idle handler; exposed for tests and
  // shutdown plumbing.
  void Shutdown();

  // Temporarily suppress idle-recycle while a background shell is running.
  // Keepalive callers use opaque shell ids as tokens; repeated retain/release is
  // idempotent. Once the last token is released, the normal 30s idle timer can
  // recycle the utility process again.
  void RetainBackgroundShell(const std::string& shell_id);
  void ReleaseBackgroundShell(const std::string& shell_id);

  // Browser-side terminal-result cache. The utility process owns the live child,
  // but its in-memory registry is intentionally short-lived; caching terminal
  // reads here lets manual BashOutput recover output after the service later
  // idle-recycles or restarts.
  struct CachedBackgroundOutput {
    CachedBackgroundOutput();
    CachedBackgroundOutput(const CachedBackgroundOutput&);
    CachedBackgroundOutput& operator=(const CachedBackgroundOutput&);
    CachedBackgroundOutput(CachedBackgroundOutput&&);
    CachedBackgroundOutput& operator=(CachedBackgroundOutput&&);
    ~CachedBackgroundOutput();

    int exit_code = -1;
    std::optional<int> signal;
    bool timed_out = false;
    bool killed = false;
    uint64_t duration_ms = 0;
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
  };
  void CacheBackgroundOutput(const std::string& shell_id,
                             CachedBackgroundOutput output);
  std::optional<CachedBackgroundOutput> GetCachedBackgroundOutput(
      const std::string& shell_id) const;

  // Launch a background shell AND register for its terminal exit notification.
  // Wraps the mojo `SpawnWithObserver`: the launcher binds a
  // `NexusBackgroundShellObserver` receiver, hands the remote to the utility,
  // and — on the reply — keeps the receiver alive keyed by `shell_id` so the
  // utility's eventual `OnExit` lands here. `reply` mirrors the plain `Spawn`
  // response (ok / shell_id / error_*). On success the launcher broadcasts the
  // exit to all registered event sinks (see `AddShellEventSink`).
  void SpawnBackgroundWithObserver(mojom::ShellSpawnOptionsPtr options,
                                   SpawnReplyCallback reply);

  // Register a bridge guest's event sink. The launcher broadcasts every
  // background-shell terminal exit to ALL registered sinks (each guest filters
  // by shellId on the JS side). Registering at the BRIDGE level (not per-shell)
  // avoids a spawn/poll race: a shell is spawned by ProcessTool, which has no
  // bridge delegate, so the exit listener must exist before the guest's first
  // shell.status — a global sink registered when the bridge attaches covers
  // every shell regardless of which one the guest polls first. Returns a
  // subscription id; pass it to `RemoveShellEventSink` when the guest detaches.
  uint64_t AddShellEventSink(ShellEventSink sink);
  void RemoveShellEventSink(uint64_t subscription_id);

  bool IsBackgroundShellRetainedForTesting(const std::string& shell_id) const;
  size_t retained_background_shell_count_for_testing() const;
  size_t cached_background_output_count_for_testing() const;

  base::WeakPtr<NexusShellServiceLauncher> GetWeakPtr();

  // Idle timeout used by the production launcher. Exposed for tests so
  // they can assert the value the LLM-facing docs reference.
  static constexpr base::TimeDelta kIdleTimeout = base::Seconds(30);

 private:
  void ConfigureIdleHandler();

  // One mojo receiver per watched background shell (keyed by shell_id). Bound
  // in `SpawnBackgroundWithObserver`, kept alive until the utility fires
  // `OnExit` (which resets it) so the terminal notification is never dropped.
  std::map<std::string,
           std::unique_ptr<mojo::Receiver<mojom::NexusBackgroundShellObserver>>>
      exit_receivers_;
  // Connected bridge guests' event sinks, keyed by subscription id. `OnExit`
  // broadcasts the terminal exit to every sink; each guest filters by shellId
  // on the JS side. Global (not per-shell) so a shell spawned before the
  // guest's first shell.status is still covered.
  std::map<uint64_t, ShellEventSink> event_sinks_;
  uint64_t next_sink_id_ = 1;

  NexusUtilityServiceRemote<mojom::NexusShellService> remote_;
  std::set<std::string> retained_background_shells_;
  std::map<std::string, CachedBackgroundOutput> cached_background_outputs_;
  std::deque<std::string> cached_background_output_order_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusShellServiceLauncher> weak_factory_{this};
};

// Returns the per-Profile SHARED launcher (lazily created, stored as Profile
// user-data). `ProcessTool` (which spawns background shells via `Spawn`) AND the
// guest `shell.*` bridge adapter (which observes/kills them via `shell.status` /
// `shell.kill`) both call this, so they funnel through ONE utility process and
// thus ONE background-shell registry — without it, a separate launcher would
// spawn a separate utility process whose registry can't see the other's shells.
// UI-thread only (the returned launcher is sequence-bound). The launcher lives
// as long as the Profile; its 30s idle-recycle still applies when no background
// shell ids are retained. Background Spawn callers retain their shell id until a
// terminal shell.status / BashOutput read releases it, so a long-running shell's
// in-memory registry is not lost just because the model waited before reading.
NexusShellServiceLauncher& GetSharedShellLauncher(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SHELL_SERVICE_LAUNCHER_H_
