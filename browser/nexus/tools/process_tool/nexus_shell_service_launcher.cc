// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/supports_user_data.h"
#include "base/task/single_thread_task_runner.h"
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
  // The utility process is going away — any pending exit receivers will never
  // fire. Drop them so a later relaunch starts clean (a still-running shell's
  // JS side falls back to its safety poll to learn the outcome). Event sinks
  // survive (they belong to the bridge guests, not the utility connection).
  exit_receivers_.clear();
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

void NexusShellServiceLauncher::SpawnBackgroundWithObserver(
    mojom::ShellSpawnOptionsPtr options,
    SpawnReplyCallback reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Bind a fresh observer receiver to THIS launcher; the utility will call
  // OnExit on it when the child terminates. The receiver is parked (unbound to
  // a shell_id) until the spawn reply returns the shell_id, at which point it
  // is keyed into `exit_receivers_` so it stays alive until OnExit lands.
  auto receiver =
      std::make_unique<mojo::Receiver<mojom::NexusBackgroundShellObserver>>(
          this);
  mojo::PendingRemote<mojom::NexusBackgroundShellObserver> observer_remote =
      receiver->BindNewPipeAndPassRemote();

  GetService()->SpawnWithObserver(
      std::move(options), std::move(observer_remote),
      base::BindOnce(
          [](base::WeakPtr<NexusShellServiceLauncher> self,
             std::unique_ptr<mojo::Receiver<mojom::NexusBackgroundShellObserver>>
                 receiver,
             SpawnReplyCallback reply, bool ok,
             const std::optional<std::string>& shell_id,
             const std::optional<std::string>& error_code,
             const std::optional<std::string>& error_message,
             const std::optional<std::string>& install_hint) {
            if (self && ok && shell_id.has_value() && !shell_id->empty()) {
              // Key the receiver by shell_id and keep it alive until OnExit.
              // If the browser side drops (launcher teardown), the disconnect
              // handler reaps the now-orphaned receiver.
              const std::string id = *shell_id;
              receiver->set_disconnect_handler(base::BindOnce(
                  [](base::WeakPtr<NexusShellServiceLauncher> self,
                     std::string shell_id) {
                    if (!self) return;
                    self->exit_receivers_.erase(shell_id);
                  },
                  self, id));
              self->exit_receivers_[id] = std::move(receiver);
            }
            // Pre-launch rejection (or launcher gone): the receiver is dropped
            // here and the observer simply never fires.
            std::move(reply).Run(ok, shell_id, error_code, error_message,
                                 install_hint);
          },
          weak_factory_.GetWeakPtr(), std::move(receiver), std::move(reply)));
}

void NexusShellServiceLauncher::OnExit(const std::string& shell_id,
                                       mojom::ShellExitInfoPtr exit) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The utility fires OnExit exactly once; the receiver has served its purpose.
  // Erase it to bound memory — but NOT inline: this runs inside that receiver's
  // dispatch, and destroying the mojo::Receiver mid-callback is unsafe. Post the
  // erase so it lands after the dispatch unwinds. `Shutdown()` also clears them
  // all on utility recycle, and the disconnect handler reaps a dropped pipe.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NexusShellServiceLauncher> self, std::string id) {
            if (self) self->exit_receivers_.erase(id);
          },
          weak_factory_.GetWeakPtr(), shell_id));
  if (!exit) return;

  // Broadcast the terminal exit to every connected bridge guest. Each guest
  // filters by shellId on the JS side (only the owning session finalizes the
  // task). Snapshot the sinks first so a re-entrant RemoveShellEventSink from
  // within a sink callback can't invalidate the iteration.
  std::vector<ShellEventSink> sinks;
  sinks.reserve(event_sinks_.size());
  for (const auto& [id, sink] : event_sinks_) {
    sinks.push_back(sink);
  }
  for (const auto& sink : sinks) {
    sink.Run(shell_id, *exit);
  }
}

uint64_t NexusShellServiceLauncher::AddShellEventSink(ShellEventSink sink) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t id = next_sink_id_++;
  event_sinks_[id] = std::move(sink);
  return id;
}

void NexusShellServiceLauncher::RemoveShellEventSink(uint64_t subscription_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  event_sinks_.erase(subscription_id);
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