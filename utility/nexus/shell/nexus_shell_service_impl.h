// Copyright 2026 VellocLabs
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

// Implementation of `nexus.mojom.NexusShellService` in a utility process.
// Probes and child invocations block, so they run on MayBlock tasks; the Mojo
// sequence only routes. `GetCapabilities` is cached for the process lifetime
// and requests before the first probe are queued.
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

  // Background `Spawn` helpers. `RunSpawn` runs the same validation /
  // allowlist / cwd checks as `RunExec` (via the shared `PrepareExec`
  // free function), allocates a `shell_id`, replies immediately, then
  // posts the (blocking) child-process invocation to the thread pool.
  // `OnBackgroundComplete` stores the outcome into `background_` on the
  // binding sequence — so the registry is only ever touched there and
  // needs no lock.
  void RunSpawn(mojom::ShellSpawnOptionsPtr options, SpawnCallback callback);
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
  // the partial output so far instead of empty buffers until exit.
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
  std::vector<PendingStreamSpawn> pending_stream_spawns_;

  // Background processes, keyed by the opaque `shell_id` returned from `Spawn`.
  // Entries persist after exit so the model can poll the final output;
  // capped at `kMaxBackgroundEntries` (oldest evicted) to bound memory.
  std::map<std::string, BackgroundEntry> background_;

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
