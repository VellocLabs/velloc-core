// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_NEXUS_SHELL_SERVICE_IMPL_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_NEXUS_SHELL_SERVICE_IMPL_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/shell/shell_resolver.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

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

  mojo::Receiver<mojom::NexusShellService> receiver_{this};

  // Populated by `OnResolved`. `has_value()` is the "probe finished"
  // signal; we never invalidate the cache.
  std::optional<ResolvedCapabilities> cached_capabilities_;

  // Callbacks waiting for the in-flight probe to finish. Drained by
  // `OnResolved` in FIFO order.
  std::vector<GetCapabilitiesCallback> pending_callbacks_;
  std::vector<PendingExec> pending_execs_;

  bool resolve_in_flight_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusShellServiceImpl> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_NEXUS_SHELL_SERVICE_IMPL_H_
