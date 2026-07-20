// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_stream_guard.h"

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"

namespace nexus {

namespace {

// How often the idle watchdog re-checks a live stream. Much finer than the
// timeout itself so a hung stream is caught promptly, coarse enough to be
// cheap. Not configurable — the timeout is the user-facing knob.
constexpr base::TimeDelta kIdleCheckInterval = base::Seconds(15);

}  // namespace

LlmStreamGuard::QueuedStream::QueuedStream() = default;
LlmStreamGuard::QueuedStream::QueuedStream(QueuedStream&&) = default;
LlmStreamGuard::QueuedStream& LlmStreamGuard::QueuedStream::operator=(
    QueuedStream&&) = default;
LlmStreamGuard::QueuedStream::~QueuedStream() = default;

LlmStreamGuard::LlmStreamGuard(std::string provider_id)
    : provider_id_(std::move(provider_id)),
      stream_idle_timeout_(base::Seconds(180)),
      max_concurrent_streams_(0) {}

LlmStreamGuard::~LlmStreamGuard() = default;

bool LlmStreamGuard::CanStartStream() const {
  if (max_concurrent_streams_ <= 0) {
    return true;  // Unlimited.
  }
  return active_stream_count() < static_cast<size_t>(max_concurrent_streams_);
}

void LlmStreamGuard::EnqueueStream(QueuedStream stream) {
  queue_.push_back(std::move(stream));
}

std::optional<LlmStreamGuard::QueuedStream> LlmStreamGuard::DequeueStream() {
  if (queue_.empty()) {
    return std::nullopt;
  }
  QueuedStream stream = std::move(queue_.front());
  queue_.pop_front();
  return stream;
}

void LlmStreamGuard::TrackStream(const std::string& request_id) {
  auto state = std::make_unique<StreamState>();
  state->last_activity = base::TimeTicks::Now();
  ArmIdleTimer(*state, request_id);
  streams_.emplace(request_id, std::move(state));
}

void LlmStreamGuard::TrackBlocking(const std::string& request_id) {
  auto state = std::make_unique<StreamState>();
  state->last_activity = base::TimeTicks::Now();
  // No ArmIdleTimer: blocking requests have no per-chunk idle signal, so the
  // watchdog would fire spuriously on any long one-shot download. The slot is
  // released via OnStreamFinished (success/error/cancel) only.
  streams_.emplace(request_id, std::move(state));
}

void LlmStreamGuard::NoteStreamBytes(const std::string& request_id) {
  auto it = streams_.find(request_id);
  if (it != streams_.end()) {
    it->second->last_activity = base::TimeTicks::Now();
  }
}

void LlmStreamGuard::OnStreamFinished(const std::string& request_id) {
  streams_.erase(request_id);
}

void LlmStreamGuard::ArmIdleTimer(StreamState& state,
                                  const std::string& request_id) {
  state.idle_timer.Start(FROM_HERE, kIdleCheckInterval,
                         base::BindOnce(&LlmStreamGuard::OnIdleCheck,
                                        weak_factory_.GetWeakPtr(), request_id));
}

void LlmStreamGuard::OnIdleCheck(const std::string& request_id) {
  auto it = streams_.find(request_id);
  if (it == streams_.end()) {
    return;
  }
  const base::TimeDelta idle =
      base::TimeTicks::Now() - it->second->last_activity;
  if (idle < stream_idle_timeout_) {
    // Still alive — re-arm and wait for the next check.
    ArmIdleTimer(*it->second, request_id);
    return;
  }
  // Hung: no bytes for the full timeout. Erase first so the provider's idle
  // handler (which fails + finishes the stream) doesn't double-release the
  // slot, then fire the provider's idle callback.
  streams_.erase(it);
  if (on_stream_idle_) {
    on_stream_idle_.Run(request_id);
  }
}

std::string LlmStreamGuard::BuildDebugString() const {
  return base::StringPrintf(
      "provider=%s active=%zu/%s queued=%zu idle_timeout_s=%d",
      provider_id_.c_str(), active_stream_count(),
      max_concurrent_streams_ <= 0
          ? "unlimited"
          : base::NumberToString(max_concurrent_streams_).c_str(),
      queued_stream_count(),
      static_cast<int>(stream_idle_timeout_.InSeconds()));
}

}  // namespace nexus
