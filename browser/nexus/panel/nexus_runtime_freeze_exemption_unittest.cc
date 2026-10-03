// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A guard on UPSTREAM behaviour the Nexus runtime depends on. No Nexus C++ is
// exercised here; what is pinned is a promise Chromium makes and the guest
// relies on.
//
// The guest page is the runtime's client (its SharedWorker runs the agent
// loop) and is hidden much of the time while still working: the window is
// minimized or covered, or closed and the page parked by
// NexusBackgroundPanelHost. performance_manager's FreezingPolicy does not
// exempt a page for not being a tab — with Battery Saver active, a hidden page
// whose browsing instance was CPU-intensive in the background is frozen, and a
// SharedWorker with no active client left is frozen with it
// (SharedWorkerHost::OnClientStateChanged). A running turn would just stop.
//
// The guest prevents that without any browser change: it holds a Web Lock for
// its whole life (nexus_web: src/browser/boot/runtimeKeepAlive.ts). A frame
// holding a Web Lock gives its page CannotFreezeReason::kHoldingWebLock, and
// FreezingPolicy consults CannotFreezeReasonsForType() before every freeze.
//
// That is the contract. If a Chromium rebase ever removes kHoldingWebLock from
// a freezing type's applicable reasons, the guest's keep-alive silently stops
// working for that type — nothing else in the tree would fail. This does.

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
