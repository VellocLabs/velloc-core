// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "content/public/browser/service_process_host.h"

namespace nexus {

NexusShellServiceLauncher::NexusShellServiceLauncher() = default;

NexusShellServiceLauncher::~NexusShellServiceLauncher() {
  Shutdown();
}

mojo::Remote<mojom::NexusShellService>&
NexusShellServiceLauncher::GetService() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  EnsureLaunched();
  return remote_;
}

void NexusShellServiceLauncher::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  remote_.reset();
}

void NexusShellServiceLauncher::EnsureLaunched() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (remote_.is_bound() && remote_.is_connected()) {
    return;
  }

  // Drop any stale binding from a previous launch that disconnected
  // (utility crash, idle timeout, etc.) before creating a new one.
  remote_.reset();

  // The launched utility process inherits the sandbox type declared on
  // the mojom interface itself (see `[ServiceSandbox=...]` annotation
  // on `nexus.mojom.NexusShellService`).
  //
  // The display name is intentionally hard-coded ASCII rather than an
  // IDS_ resource id: we want this launcher to compile in
  // `custom_browser` without forcing every dependent layer to add a
  // string-resource entry. If/when we want a localised name in the
  // task manager, swap to `WithDisplayName(IDS_NEXUS_SHELL_SERVICE_NAME)`
  // and add the string upstream — see the upstream-hook report.
  remote_ = content::ServiceProcessHost::Launch<mojom::NexusShellService>(
      content::ServiceProcessHost::Options()
          .WithDisplayName("Nexus Shell Service")
          .Pass());

  remote_.set_disconnect_handler(base::BindOnce(
      &NexusShellServiceLauncher::OnDisconnect, base::Unretained(this)));

  // Idle-recycle: after `kIdleTimeout` of no calls, drop the remote so
  // the utility process exits. The next `GetService()` will relaunch.
  remote_.set_idle_handler(
      kIdleTimeout, base::BindRepeating(
                        [](NexusShellServiceLauncher* self) {
                          LOG(INFO) << "[NexusShellServiceLauncher] Idle "
                                       "timeout; recycling utility process";
                          self->Shutdown();
                        },
                        base::Unretained(this)));
}

void NexusShellServiceLauncher::OnDisconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(WARNING) << "[NexusShellServiceLauncher] Utility process disconnected; "
                  "next call will relaunch";
  remote_.reset();
}

}  // namespace nexus
