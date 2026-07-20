// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"

namespace nexus {

namespace {

constexpr size_t kMaxCachedBackgroundOutputs = 100;

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

NexusShellServiceLauncher::CachedBackgroundOutput::CachedBackgroundOutput() =
    default;
NexusShellServiceLauncher::CachedBackgroundOutput::CachedBackgroundOutput(
    const CachedBackgroundOutput&) = default;
NexusShellServiceLauncher::CachedBackgroundOutput&
NexusShellServiceLauncher::CachedBackgroundOutput::operator=(
    const CachedBackgroundOutput&) = default;
NexusShellServiceLauncher::CachedBackgroundOutput::CachedBackgroundOutput(
    CachedBackgroundOutput&&) = default;
NexusShellServiceLauncher::CachedBackgroundOutput&
NexusShellServiceLauncher::CachedBackgroundOutput::operator=(
    CachedBackgroundOutput&&) = default;
NexusShellServiceLauncher::CachedBackgroundOutput::~CachedBackgroundOutput() =
    default;

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

void NexusShellServiceLauncher::CacheBackgroundOutput(
    const std::string& shell_id,
    CachedBackgroundOutput output) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shell_id.empty()) return;
  if (!cached_background_outputs_.contains(shell_id)) {
    cached_background_output_order_.push_back(shell_id);
  }
  cached_background_outputs_[shell_id] = std::move(output);
  while (cached_background_outputs_.size() > kMaxCachedBackgroundOutputs &&
         !cached_background_output_order_.empty()) {
    std::string oldest = cached_background_output_order_.front();
    cached_background_output_order_.pop_front();
    if (oldest == shell_id) {
      // The entry just written is at the front — keep map and order deque in
      // sync by re-appending it as most-recently-used instead of dropping it
      // from the deque (which would make it un-evictable forever).
      cached_background_output_order_.push_back(std::move(oldest));
      continue;
    }
    cached_background_outputs_.erase(oldest);
  }
}

std::optional<NexusShellServiceLauncher::CachedBackgroundOutput>
NexusShellServiceLauncher::GetCachedBackgroundOutput(
    const std::string& shell_id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = cached_background_outputs_.find(shell_id);
  if (it == cached_background_outputs_.end()) return std::nullopt;
  return it->second;
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

size_t NexusShellServiceLauncher::cached_background_output_count_for_testing()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return cached_background_outputs_.size();
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