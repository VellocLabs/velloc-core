// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Guards upstream behaviour the guest relies on: a frame holding a Web Lock
// gets CannotFreezeReason::kHoldingWebLock, which FreezingPolicy honours, so a
// hidden guest page (and its SharedWorker) is never frozen mid-turn.

#include "components/performance_manager/freezing/freezing_policy.h"

#include "components/performance_manager/public/freezing/cannot_freeze_reason.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

using performance_manager::FreezingPolicy;
using performance_manager::freezing::CannotFreezeReason;

// Every way FreezingPolicy can decide to freeze a page: an embedder's freeze
// vote, Battery Saver on a CPU-intensive background page, and (behind a
// feature) the infinite-tabs policy. A new FreezingType must be added here —
// and to the question of whether the guest's lock covers it.
constexpr FreezingPolicy::FreezingType kEveryFreezingType[] = {
    FreezingPolicy::FreezingType::kVoting,
    FreezingPolicy::FreezingType::kBatterySaver,
    FreezingPolicy::FreezingType::kInfiniteTabs,
};

TEST(NexusRuntimeFreezeExemptionTest, AHeldWebLockBlocksEveryFreezingType) {
  for (FreezingPolicy::FreezingType type : kEveryFreezingType) {
    EXPECT_TRUE(FreezingPolicy::CannotFreezeReasonsForType(type).Has(
        CannotFreezeReason::kHoldingWebLock))
        << "FreezingType " << static_cast<int>(type)
        << " no longer honours a held Web Lock: the Nexus guest's runtime "
           "keep-alive (runtimeKeepAlive.ts) does not protect it from this "
           "kind of freeze any more.";
  }
}

// The reason the lock is needed at all: nothing about the guest being hidden
// is, by itself, a reason not to freeze it. kVisible only protects a page
// while it is visible, and a parked page never is.
TEST(NexusRuntimeFreezeExemptionTest, BatterySaverFreezingHasNoHiddenPageExemption) {
  const auto reasons = FreezingPolicy::CannotFreezeReasonsForType(
      FreezingPolicy::FreezingType::kBatterySaver);
  EXPECT_TRUE(reasons.Has(CannotFreezeReason::kVisible));
  // The tab-only protection is explicitly NOT applicable to Battery Saver —
  // and the guest is not a tab anyway.
  EXPECT_FALSE(reasons.Has(CannotFreezeReason::kMostRecentlyUsed));
}

}  // namespace
}  // namespace nexus
