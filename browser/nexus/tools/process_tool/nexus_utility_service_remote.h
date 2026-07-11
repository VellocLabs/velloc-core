// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_UTILITY_SERVICE_REMOTE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_UTILITY_SERVICE_REMOTE_H_

#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "content/public/browser/service_process_host.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

// Shared browser-side utility-process remote lifecycle for Nexus services.
// Service-specific launchers keep policy/state (e.g. retained background shell
// ids), while this helper owns the repetitive ServiceProcessHost launch,
// disconnect, and idle-recycle plumbing.
template <typename Service>
class NexusUtilityServiceRemote {
 public:
  NexusUtilityServiceRemote(std::string_view display_name,
                            std::string_view log_prefix,
                            base::TimeDelta idle_timeout)
      : display_name_(display_name),
        log_prefix_(log_prefix),
        idle_timeout_(idle_timeout) {}

  NexusUtilityServiceRemote(const NexusUtilityServiceRemote&) = delete;
  NexusUtilityServiceRemote& operator=(const NexusUtilityServiceRemote&) = delete;

  ~NexusUtilityServiceRemote() { Shutdown(); }

  mojo::Remote<Service>& GetService() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    EnsureLaunched();
    return remote_;
  }

  void Shutdown() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    remote_.reset();
  }

  bool is_bound() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return remote_.is_bound();
  }

  void ConfigureIdleHandler(base::RepeatingClosure on_idle) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!remote_.is_bound()) return;
    remote_.set_idle_handler(idle_timeout_, std::move(on_idle));
  }

 private:
  void EnsureLaunched() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (remote_.is_bound() && remote_.is_connected()) return;
    remote_.reset();
    remote_ = content::ServiceProcessHost::Launch<Service>(
        content::ServiceProcessHost::Options()
            .WithDisplayName(std::string(display_name_))
            .Pass());
    remote_.set_disconnect_handler(base::BindOnce(
        &NexusUtilityServiceRemote::OnDisconnect, base::Unretained(this)));
    ConfigureIdleHandler(base::BindRepeating(
        [](NexusUtilityServiceRemote* self) {
          LOG(INFO) << "[" << self->log_prefix_ << "] Idle timeout; recycling";
          self->Shutdown();
        },
        base::Unretained(this)));
  }

  void OnDisconnect() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    LOG(WARNING) << "[" << log_prefix_
                 << "] Utility process disconnected; next call will relaunch";
    remote_.reset();
  }

  std::string_view display_name_;
  std::string_view log_prefix_;
  base::TimeDelta idle_timeout_;
  mojo::Remote<Service> remote_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_UTILITY_SERVICE_REMOTE_H_
