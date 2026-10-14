// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"

namespace nexus {

namespace {

class SharedShellLauncherUserData : public base::SupportsUserData::Data {
 public:
  NexusShellServiceLauncher launcher;
};

const char kSharedShellLauncherKey[] = "nexus::SharedShellLauncher";

}  // namespace

NexusShellServiceLauncher& GetSharedShellLauncher(Profile* profile) {
  CHECK(profile);
  auto* data = static_cast<SharedShellLauncherUserData*>(
      profile->GetUserData(kSharedShellLauncherKey));
  if (!data) {
    auto owned = std::make_unique<SharedShellLauncherUserData>();
    data = owned.get();
    profile->SetUserData(kSharedShellLauncherKey, std::move(owned));
  }
  return data->launcher;
}

NexusShellServiceLauncher::NexusShellServiceLauncher()
    : remote_("Nexus Shell Service", "NexusShellServiceLauncher",
              kIdleTimeout) {}

NexusShellServiceLauncher::~NexusShellServiceLauncher() {
  Shutdown();
}

mojo::Remote<mojom::NexusShellService>& NexusShellServiceLauncher::GetService() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return remote_.GetService();
}

void NexusShellServiceLauncher::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  remote_.Shutdown();
}

void NexusShellServiceLauncher::RetainBackgroundShell(
    const std::string& shell_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shell_id.empty()) return;
  const bool was_empty = retained_background_shells_.empty();
  retained_background_shells_.insert(shell_id);
  if (was_empty && remote_.is_bound()) ConfigureIdleHandler();
}

void NexusShellServiceLauncher::ReleaseBackgroundShell(
    const std::string& shell_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shell_id.empty()) return;
  const size_t erased = retained_background_shells_.erase(shell_id);
  if (erased > 0 && retained_background_shells_.empty() && remote_.is_bound()) {
    ConfigureIdleHandler();
  }
}

bool NexusShellServiceLauncher::IsBackgroundShellRetainedForTesting(
    const std::string& shell_id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return retained_background_shells_.contains(shell_id);
}

size_t NexusShellServiceLauncher::retained_background_shell_count_for_testing()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return retained_background_shells_.size();
}

base::WeakPtr<NexusShellServiceLauncher> NexusShellServiceLauncher::GetWeakPtr() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return weak_factory_.GetWeakPtr();
}

void NexusShellServiceLauncher::ConfigureIdleHandler() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!remote_.is_bound()) return;
  remote_.ConfigureIdleHandler(base::BindRepeating(
      [](NexusShellServiceLauncher* self) {
        if (!self->retained_background_shells_.empty()) {
          LOG(INFO) << "[NexusShellServiceLauncher] Idle timeout ignored; "
                    << self->retained_background_shells_.size()
                    << " background shell(s) retained";
          return;
        }
        LOG(INFO) << "[NexusShellServiceLauncher] Idle timeout; recycling utility process";
        self->Shutdown();
      },
      base::Unretained(this)));
}

}  // namespace nexus