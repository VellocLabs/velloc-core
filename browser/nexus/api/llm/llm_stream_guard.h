// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_STREAM_GUARD_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_STREAM_GUARD_H_

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"

namespace nexus {

// Generic per-provider stream guard: idle-timeout watchdog + concurrency
// pool, plus a debug string for the /debug diagnostic pipeline. Provider
// agnostic — a concrete provider (OpenAI today; Anthropic/Google/Ollama
// later, each with a different loader substrate) composes one instance and
// routes its stream lifecycle through it.
//
// Two independent knobs, both with defaults that preserve prior behaviour:
//   - stream_idle_timeout (default 180s): a stream that receives NO bytes for
//     this long is treated as hung and the guard's idle callback fires. Bytes
//     of ANY kind (real chunks, SSE comment/keep-alive lines, blank
//     keepalives) refresh the activity clock, so a reasoning model's long
//     thinking phase is NOT killed while the server keeps the connection
//     warm.
//   - max_concurrent_streams (default 0 = unlimited): when positive, streams
//     beyond the cap are queued FIFO and started only when an active slot
//     frees. The ChatGPT-subscription (Codex) backend enforces a per-account
//     concurrency ceiling and silently hangs over-quota connections; capping
//     chatgpt-pro to 1 (via provider config) keeps it serial.
//
// Sequenced: all methods must run on the provider's sequence.
class LlmStreamGuard {
 public:
  // Fires once per hung stream with its request_id. The provider fails that
  // stream (runs its done_callback with an error) and releases its slot.
  using OnStreamIdleCallback =
      base::RepeatingCallback<void(const std::string& request_id)>;

  // One queued stream, held until a concurrency slot frees.
  struct QueuedStream {
    QueuedStream();
    QueuedStream(QueuedStream&&);
    QueuedStream& operator=(QueuedStream&&);
    ~QueuedStream();

    std::string request_id;
    base::DictValue call_options;
    base::RepeatingCallback<void(base::DictValue)> chunk_callback;
    base::OnceCallback<void(std::optional<std::string>)> done_callback;
  };

  // `provider_id` is used only for the debug string.
  explicit LlmStreamGuard(std::string provider_id);
  ~LlmStreamGuard();

  LlmStreamGuard(const LlmStreamGuard&) = delete;
  LlmStreamGuard& operator=(const LlmStreamGuard&) = delete;

  // --- Configuration (called from the provider's Configure()) ---
  void set_stream_idle_timeout(base::TimeDelta t) { stream_idle_timeout_ = t; }
  void set_max_concurrent_streams(int n) { max_concurrent_streams_ = n; }
  base::TimeDelta stream_idle_timeout() const { return stream_idle_timeout_; }
  int max_concurrent_streams() const { return max_concurrent_streams_; }

  // Single idle handler for all tracked streams (the provider binds its own
  // weak method). Set once at construction time in the provider.
  void set_on_stream_idle(OnStreamIdleCallback cb) {
    on_stream_idle_ = std::move(cb);
  }

  // --- Concurrency pool ---
  // True when a new stream may start immediately (under the cap / unlimited).
  bool CanStartStream() const;
  void EnqueueStream(QueuedStream stream);
  // Pops the next queued stream, or nullopt when empty. Call after a slot
  // frees (i.e. from the provider's stream-finished path).
  std::optional<QueuedStream> DequeueStream();

  // Registers a newly-started stream: begins its idle watchdog and counts it
  // as active.
  void TrackStream(const std::string& request_id);
  // Records inbound bytes — refreshes the stream's idle clock.
  void NoteStreamBytes(const std::string& request_id);
  // Marks a stream finished (success, error, cancel, or idle-timeout): stops
  // its watchdog and frees its slot. Safe to call for unknown ids.
  void OnStreamFinished(const std::string& request_id);

  size_t active_stream_count() const { return streams_.size(); }
  size_t queued_stream_count() const { return queue_.size(); }

  // Single-line diagnostic for the /debug pipeline, e.g.:
  //   provider=chatgpt-pro active=1/1 queued=2 idle_timeout_s=180
  std::string BuildDebugString() const;

 private:
  struct StreamState {
    base::TimeTicks last_activity;
    base::OneShotTimer idle_timer;
  };

  void OnIdleCheck(const std::string& request_id);
  void ArmIdleTimer(StreamState& state, const std::string& request_id);

  const std::string provider_id_;
  base::TimeDelta stream_idle_timeout_;
  int max_concurrent_streams_;  // 0 = unlimited.
  OnStreamIdleCallback on_stream_idle_;

  std::map<std::string, std::unique_ptr<StreamState>> streams_;
  std::deque<QueuedStream> queue_;

  base::WeakPtrFactory<LlmStreamGuard> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_STREAM_GUARD_H_
