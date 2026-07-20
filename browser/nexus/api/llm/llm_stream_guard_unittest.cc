// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_stream_guard.h"

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Deterministic coverage for the stream guard's two knobs, using a mock task
// runner so the idle watchdog can be "fast-forwarded" instead of wall-clock
// waiting the real 15s check interval / 180s default.
class LlmStreamGuardTest : public ::testing::Test {
 protected:
  LlmStreamGuardTest()
      : task_environment_(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  base::test::TaskEnvironment task_environment_;
};

TEST_F(LlmStreamGuardTest, DefaultsAreUnlimitedConcurrencyAnd180sIdle) {
  LlmStreamGuard guard("openai");
  EXPECT_EQ(guard.max_concurrent_streams(), 0);
  EXPECT_EQ(guard.stream_idle_timeout(), base::Seconds(180));
  EXPECT_TRUE(guard.CanStartStream());
}

TEST_F(LlmStreamGuardTest, ConcurrencyPoolQueuesOverCapAndDrainsFifo) {
  LlmStreamGuard guard("openai");
  guard.set_max_concurrent_streams(1);

  guard.TrackStream("a");
  EXPECT_TRUE(guard.CanStartStream() == false);
  EXPECT_EQ(guard.active_stream_count(), 1u);

  // Over the cap: enqueue two, they must come back FIFO.
  LlmStreamGuard::QueuedStream q1;
  q1.request_id = "b";
  LlmStreamGuard::QueuedStream q2;
  q2.request_id = "c";
  guard.EnqueueStream(std::move(q1));
  guard.EnqueueStream(std::move(q2));
  EXPECT_EQ(guard.queued_stream_count(), 2u);

  // Free a slot: first dequeue returns "b".
  guard.OnStreamFinished("a");
  auto next = guard.DequeueStream();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->request_id, "b");
  guard.TrackStream(next->request_id);
  EXPECT_EQ(guard.active_stream_count(), 1u);

  // Free again: "c".
  guard.OnStreamFinished("b");
  next = guard.DequeueStream();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->request_id, "c");
  EXPECT_EQ(guard.queued_stream_count(), 0u);
}

TEST_F(LlmStreamGuardTest, UnlimitedConcurrencyNeverBlocks) {
  LlmStreamGuard guard("openai");
  guard.set_max_concurrent_streams(0);  // Unlimited.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(guard.CanStartStream());
    guard.TrackStream("s" + std::to_string(i));
  }
  EXPECT_EQ(guard.active_stream_count(), 5u);
}

TEST_F(LlmStreamGuardTest, IdleTimeoutFiresAfterQuietPeriod) {
  LlmStreamGuard guard("openai");
  guard.set_stream_idle_timeout(base::Seconds(20));

  std::string idle_fired_for;
  guard.set_on_stream_idle(base::BindRepeating(
      [](std::string* out, const std::string& request_id) { *out = request_id; },
      &idle_fired_for));

  guard.TrackStream("a");
  // The watchdog checks every 15s; a 20s timeout should trip on the second
  // check (~30s). Fast-forward well past that.
  task_environment_.FastForwardBy(base::Seconds(35));
  EXPECT_EQ(idle_fired_for, "a");
  // The stream is removed from tracking after the timeout fires.
  EXPECT_EQ(guard.active_stream_count(), 0u);
}

TEST_F(LlmStreamGuardTest, InboundBytesRefreshIdleClock) {
  LlmStreamGuard guard("openai");
  guard.set_stream_idle_timeout(base::Seconds(20));

  std::string idle_fired_for;
  guard.set_on_stream_idle(base::BindRepeating(
      [](std::string* out, const std::string& request_id) { *out = request_id; },
      &idle_fired_for));

  guard.TrackStream("a");
  // Keep the stream alive with bytes every ~14s (before each 15s check sees a
  // 20s+ idle gap): 14s then bytes, 14s then bytes, ... so it never times out
  // across 60s of simulated time.
  for (int i = 0; i < 4; ++i) {
    task_environment_.FastForwardBy(base::Seconds(14));
    guard.NoteStreamBytes("a");
  }
  EXPECT_TRUE(idle_fired_for.empty());
  EXPECT_EQ(guard.active_stream_count(), 1u);
}

TEST_F(LlmStreamGuardTest, BuildDebugStringReportsState) {
  LlmStreamGuard guard("chatgpt-pro");
  guard.set_max_concurrent_streams(1);
  guard.TrackStream("a");
  LlmStreamGuard::QueuedStream q;
  q.request_id = "b";
  guard.EnqueueStream(std::move(q));

  const std::string dbg = guard.BuildDebugString();
  EXPECT_NE(dbg.find("provider=chatgpt-pro"), std::string::npos);
  EXPECT_NE(dbg.find("active=1/1"), std::string::npos);
  EXPECT_NE(dbg.find("queued=1"), std::string::npos);
  EXPECT_NE(dbg.find("idle_timeout_s=180"), std::string::npos);
}


TEST_F(LlmStreamGuardTest, BlockingEntriesShareTheSameFifoQueue) {
  LlmStreamGuard guard("chatgpt-pro");
  guard.set_max_concurrent_streams(1);

  // One active stream occupies the only slot.
  guard.TrackStream("s1");
  EXPECT_FALSE(guard.CanStartStream());

  // A blocking request and a stream both queue behind it; FIFO order is
  // preserved across the two kinds.
  LlmStreamGuard::QueuedStream blocking;
  blocking.request_id = "g1";
  blocking.generate_callback =
      base::BindOnce([](GenerateResult, std::optional<std::string>) {});
  EXPECT_TRUE(blocking.is_blocking());

  LlmStreamGuard::QueuedStream stream;
  stream.request_id = "s2";
  EXPECT_FALSE(stream.is_blocking());

  guard.EnqueueStream(std::move(blocking));
  guard.EnqueueStream(std::move(stream));
  EXPECT_EQ(guard.queued_stream_count(), 2u);

  // Free the slot: the blocking entry comes back first.
  guard.OnStreamFinished("s1");
  auto next = guard.DequeueStream();
  ASSERT_TRUE(next.has_value());
  EXPECT_TRUE(next->is_blocking());
  EXPECT_EQ(next->request_id, "g1");
  guard.TrackBlocking(next->request_id);

  guard.OnStreamFinished("g1");
  next = guard.DequeueStream();
  ASSERT_TRUE(next.has_value());
  EXPECT_FALSE(next->is_blocking());
  EXPECT_EQ(next->request_id, "s2");
}

TEST_F(LlmStreamGuardTest, TrackBlockingCountsSlotWithoutArmingWatchdog) {
  LlmStreamGuard guard("chatgpt-pro");
  guard.set_stream_idle_timeout(base::Seconds(20));
  guard.set_max_concurrent_streams(1);

  std::string idle_fired_for;
  guard.set_on_stream_idle(base::BindRepeating(
      [](std::string* out, const std::string& request_id) { *out = request_id; },
      &idle_fired_for));

  // A blocking request occupies the slot (cap reached) ...
  guard.TrackBlocking("g1");
  EXPECT_FALSE(guard.CanStartStream());
  EXPECT_EQ(guard.active_stream_count(), 1u);

  // ... but its idle watchdog is NOT armed, so simulated time well past the
  // timeout never fires the idle callback for it.
  task_environment_.FastForwardBy(base::Seconds(60));
  EXPECT_TRUE(idle_fired_for.empty());
  EXPECT_EQ(guard.active_stream_count(), 1u);

  // The slot is released only via OnStreamFinished.
  guard.OnStreamFinished("g1");
  EXPECT_EQ(guard.active_stream_count(), 0u);
  EXPECT_TRUE(guard.CanStartStream());
}

}  // namespace
}  // namespace nexus
