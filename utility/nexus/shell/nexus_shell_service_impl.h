// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_NEXUS_SHELL_SERVICE_IMPL_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_NEXUS_SHELL_SERVICE_IMPL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"
#include "custom_browser/utility/nexus/shell/shell_resolver.h"
#include "custom_browser/utility/nexus/shell/streaming_child_process.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

// Implementation of `nexus.mojom.NexusShellService`. As of slice 2 it
// supports both capability discovery and one-shot `Exec` of allowlisted
// host commands; streaming `Spawn` arrives in slice 4.
//
// Designed to live inside a sandboxed utility process launched by the
// browser via `content::ServiceProcessHost::Launch`. Both the resolver
// probe (PATH walk + `--version`) and the per-call child-process
// invocation are blocking, so they run on `base::MayBlock()` thread-pool
// tasks; the Mojo binding sequence is reserved for routing and never
// blocks.
//
// `GetCapabilities` results are cached for the lifetime of the process.
// `Exec` reuses that same cache so it never re-probes PATH per call;
// requests that arrive before the first probe finishes are queued and
// drained once the cache is populated.
class NexusShellServiceImpl : public mojom::NexusShellService {
 public:
  // For self-owned bindings (the production path used from
  // `services::RunNexusShellService`).
  explicit NexusShellServiceImpl(
      mojo::PendingReceiver<mojom::NexusShellService> receiver);

  NexusShellServiceImpl(const NexusShellServiceImpl&) = delete;
  NexusShellServiceImpl& operator=(const NexusShellServiceImpl&) = delete;

  ~NexusShellServiceImpl() override;

  // mojom::NexusShellService:
  void GetCapabilities(GetCapabilitiesCallback callback) override;
  void Exec(mojom::ShellSpawnOptionsPtr options,
            ExecCallback callback) override;
  void Spawn(mojom::ShellSpawnOptionsPtr options,
             SpawnCallback callback) override;
  void SpawnWithObserver(
      mojom::ShellSpawnOptionsPtr options,
      mojo::PendingRemote<mojom::NexusBackgroundShellObserver> observer,
      SpawnWithObserverCallback callback) override;
  void ReadBackgroundOutput(const std::string& shell_id,
                            ReadBackgroundOutputCallback callback) override;
  void Kill(const std::string& shell_id, KillCallback callback) override;
  void StreamSpawn(
      mojom::ShellSpawnOptionsPtr options,
      mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
      mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
      StreamSpawnCallback callback) override;

 private:
  // Posts the `ShellResolver::Resolve()` probe to the thread pool the
  // first time it is needed, then drains any queued capability /
  // exec requests once the probe completes. Subsequent calls reply
  // synchronously from the cached `cached_capabilities_`.
  void EnsureResolveStarted();
  void OnResolved(ResolvedCapabilities capabilities);
  void RespondFromCache(GetCapabilitiesCallback callback) const;

  // Per-Exec helpers. `RunExec` does the policy check on the binding
  // sequence then posts the actual child-process invocation to the
  // thread pool; `OnExecComplete` marshals the worker-thread result
  // back into a Mojo response.
  void RunExec(mojom::ShellSpawnOptionsPtr options, ExecCallback callback);

  // Auto-background-on-timeout (CC parity). When
  // `ShellSpawnOptions::background_on_timeout` is set, `RunExec` routes the
  // prepared request here instead of the plain post-and-reply path: the
  // child runs under the hard backstop timeout while a binding-sequence
  // `base::OneShotTimer` counts down the caller's requested `timeout_ms`
  // ("block budget"). Whichever fires first wins — early completion
  // (`OnExecBackgroundComplete` before the timer) replies with the full
  // foreground result; the timer expiring first
  // (`OnExecBackgroundTimeout`) detaches the still-running child into the
  // `background_` registry and replies with `result.background_shell_id`,
  // and the eventual `OnExecBackgroundComplete` records the final outcome
  // there for `ReadBackgroundOutput`. Both callbacks run on the binding
  // sequence, so the `replied` flag needs no lock.
  void StartExecWithBackgroundFallback(ChildProcessRequest request,
                                       ExecCallback callback);
  void OnExecBackgroundTimeout(std::string shell_id);
  void OnExecBackgroundComplete(std::string shell_id,
                                ChildProcessOutcome outcome);

  // Background `Spawn` helpers. `RunSpawn` runs the same validation /
  // allowlist / cwd checks as `RunExec` (via the shared `PrepareExec`
  // free function), allocates a `shell_id`, replies immediately, then
  // posts the (blocking) child-process invocation to the thread pool.
  // `OnBackgroundComplete` stores the outcome into `background_` on the
  // binding sequence — so the registry is only ever touched there and
  // needs no lock.
  void RunSpawn(mojom::ShellSpawnOptionsPtr options, SpawnCallback callback);
  // Background `SpawnWithObserver` helper. Thin wrapper over `RunSpawnInternal`
  // that threads the caller's `observer` remote through so
  // `OnBackgroundComplete` can fire `NexusBackgroundShellObserver.OnExit` when
  // the child reaches a terminal state. Shares the exact validation / launch /
  // reply path as the plain `Spawn`.
  void RunSpawnWithObserver(
      mojom::ShellSpawnOptionsPtr options,
      mojo::PendingRemote<mojom::NexusBackgroundShellObserver> observer,
      SpawnWithObserverCallback callback);
  // Shared implementation for `RunSpawn` and `RunSpawnWithObserver`. When
  // `observer` is valid it is stored on the `BackgroundEntry` for the terminal
  // `OnExit` notification; an invalid (null) remote means "no observer" (the
  // plain `Spawn` path).
  void RunSpawnInternal(
      mojom::ShellSpawnOptionsPtr options,
      mojo::PendingRemote<mojom::NexusBackgroundShellObserver> observer,
      SpawnCallback callback);
  void OnBackgroundComplete(std::string shell_id,
                            base::TimeTicks dispatch_start,
                            ChildProcessOutcome outcome);
  // Binding-sequence sink for the tree-kill handle that `RunChildProcess`'s
  // `on_launched` captures at launch (bounced here via `base::BindPostTask`).
  // Stores it on the `BackgroundEntry` so `Kill` can terminate the still-running
  // process tree; if a `Kill` already arrived (`kill_requested`), terminates
  // immediately instead of storing.
  void OnBackgroundLaunched(std::string shell_id,
                            ChildProcessKillHandle kill_handle);
  // Binding-sequence sink for live stdout/stderr snapshots emitted by the
  // background runner while the child is still executing. Stores capped
  // accumulated bytes on the registry entry so ReadBackgroundOutput can return
  // CC-style partial output instead of empty buffers until exit.
  void OnBackgroundOutput(std::string shell_id,
                          bool is_stdout,
                          std::vector<uint8_t> bytes,
                          bool truncated);

  // Streaming `StreamSpawn` helpers (slice 4). `RunStreamSpawn` runs the
  // same PrepareExec validation as Exec/Spawn, then — on success —
  // constructs + starts a `StreamingChildProcess` keyed in
  // `streaming_children_`. `OnStreamingChildClosed` erases the entry when
  // the child exits or either pipe drops.
  void RunStreamSpawn(
      mojom::ShellSpawnOptionsPtr options,
      mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
      mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
      StreamSpawnCallback callback);
  void OnStreamingChildClosed(uint64_t id);

  // Pending Exec requests waiting for the first capability probe to
  // finish. Drained by `OnResolved`.
  struct PendingExec {
    PendingExec();
    ~PendingExec();
    PendingExec(const PendingExec&) = delete;
    PendingExec& operator=(const PendingExec&) = delete;
    PendingExec(PendingExec&&);
    PendingExec& operator=(PendingExec&&);

    mojom::ShellSpawnOptionsPtr options;
    ExecCallback callback;
  };

  // Pending Spawn requests waiting for the first capability probe.
  // Drained by `OnResolved`, mirroring `PendingExec`.
  struct PendingSpawn {
    PendingSpawn();
    ~PendingSpawn();
    PendingSpawn(const PendingSpawn&) = delete;
    PendingSpawn& operator=(const PendingSpawn&) = delete;
    PendingSpawn(PendingSpawn&&);
    PendingSpawn& operator=(PendingSpawn&&);

    mojom::ShellSpawnOptionsPtr options;
    SpawnCallback callback;
  };

  // Pending SpawnWithObserver requests waiting for the first capability probe.
  // Carries the move-only observer endpoint through to `RunSpawnWithObserver`.
  // Drained by `OnResolved`, mirroring `PendingSpawn`.
  struct PendingSpawnWithObserver {
    PendingSpawnWithObserver();
    ~PendingSpawnWithObserver();
    PendingSpawnWithObserver(const PendingSpawnWithObserver&) = delete;
    PendingSpawnWithObserver& operator=(const PendingSpawnWithObserver&) = delete;
    PendingSpawnWithObserver(PendingSpawnWithObserver&&);
    PendingSpawnWithObserver& operator=(PendingSpawnWithObserver&&);

    mojom::ShellSpawnOptionsPtr options;
    mojo::PendingRemote<mojom::NexusBackgroundShellObserver> observer;
    SpawnWithObserverCallback callback;
  };

  // Pending StreamSpawn requests waiting for the first capability probe.
  // Carries the move-only mojo endpoints through to `RunStreamSpawn`.
  struct PendingStreamSpawn {
    PendingStreamSpawn();
    ~PendingStreamSpawn();
    PendingStreamSpawn(const PendingStreamSpawn&) = delete;
    PendingStreamSpawn& operator=(const PendingStreamSpawn&) = delete;
    PendingStreamSpawn(PendingStreamSpawn&&);
    PendingStreamSpawn& operator=(PendingStreamSpawn&&);

    mojom::ShellSpawnOptionsPtr options;
    mojo::PendingReceiver<mojom::NexusStreamingProcess> controller;
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> client;
    StreamSpawnCallback callback;
  };

  // One tracked background process started by `Spawn`. While `running`
  // the `outcome` is empty; `OnBackgroundComplete` fills it once the
  // child exits (or is killed at timeout). Only touched on the binding
  // sequence.
  struct BackgroundEntry {
    BackgroundEntry();
    ~BackgroundEntry();
    BackgroundEntry(BackgroundEntry&&);
    BackgroundEntry& operator=(BackgroundEntry&&);

    bool running = true;
    std::optional<ChildProcessOutcome> outcome;
    base::TimeDelta duration;
    std::vector<uint8_t> stdout_so_far;
    std::vector<uint8_t> stderr_so_far;
    bool stdout_truncated_so_far = false;
    bool stderr_truncated_so_far = false;
    // Tree-kill handle captured at launch via `OnBackgroundLaunched`, used by
    // `Kill` to terminate every descendant. Reset once the child completes
    // (`OnBackgroundComplete`).
    ChildProcessKillHandle kill_handle;
    // True once `OnBackgroundLaunched` has observed the child launch and either
    // stored a kill handle or determined no kill handle is available.
    bool launch_observed = false;
    // Set by `Kill` when the child was still running. `OnBackgroundLaunched`
    // reads it to terminate a handle that lands after the stop request.
    bool kill_requested = false;
    // Set only after tree termination was actually issued. `OnBackgroundComplete`
    // reads it to force `exit.killed = true` (the runner may report a normal exit
    // because the child died from our out-of-band termination).
    bool kill_issued = false;
    // Optional exit observer supplied by `SpawnWithObserver` (the browser's
    // `NexusShellServiceLauncher`). Bound while the child runs; fired ONCE from
    // `OnBackgroundComplete` with the terminal `ShellExitInfo`, then reset.
    // Invalid (unbound) for shells started via the plain `Spawn`.
    mojo::Remote<mojom::NexusBackgroundShellObserver> observer;
  };

  // One in-flight `background_on_timeout` Exec (see
  // `StartExecWithBackgroundFallback`). Holds the caller's reply callback
  // and the block-budget timer until either the child completes or the
  // timer fires. Keyed by the reserved `shell_id`. Only touched on the
  // binding sequence — no lock.
  struct ExecBackgroundState {
    ExecBackgroundState();
    ~ExecBackgroundState();

    ExecCallback callback;
    bool replied = false;
    base::TimeTicks dispatch_start;
    std::unique_ptr<base::OneShotTimer> timer;
  };

  mojo::Receiver<mojom::NexusShellService> receiver_{this};

  // Populated by `OnResolved`. `has_value()` is the "probe finished"
  // signal; we never invalidate the cache.
  std::optional<ResolvedCapabilities> cached_capabilities_;

  // Callbacks waiting for the in-flight probe to finish. Drained by
  // `OnResolved` in FIFO order.
  std::vector<GetCapabilitiesCallback> pending_callbacks_;
  std::vector<PendingExec> pending_execs_;
  std::vector<PendingSpawn> pending_spawns_;
  std::vector<PendingSpawnWithObserver> pending_spawns_with_observer_;
  std::vector<PendingStreamSpawn> pending_stream_spawns_;

  // Background processes, keyed by the opaque `shell_id` returned from `Spawn`.
  // Entries persist after exit so the model can poll the final output;
  // capped at `kMaxBackgroundEntries` (oldest evicted) to bound memory.
  std::map<std::string, BackgroundEntry> background_;

  // In-flight `background_on_timeout` Execs, keyed by the reserved
  // opaque `shell_id` (same id scheme as `background_`). An entry lives from
  // `StartExecWithBackgroundFallback` until whichever of the child
  // completing or the block-budget timer firing resolves the reply AND the
  // worker has reported its final outcome.
  std::map<std::string, std::unique_ptr<ExecBackgroundState>> exec_background_;

  // Live streaming children started by `StreamSpawn`, keyed by an internal
  // id. Each erases itself (via `OnStreamingChildClosed`) when its child
  // exits or either mojo pipe drops.
  std::map<uint64_t, std::unique_ptr<StreamingChildProcess>>
      streaming_children_;
  uint64_t next_streaming_id_ = 1;

  bool resolve_in_flight_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusShellServiceImpl> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_NEXUS_SHELL_SERVICE_IMPL_H_
