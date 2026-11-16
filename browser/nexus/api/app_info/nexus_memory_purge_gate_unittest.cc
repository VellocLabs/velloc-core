// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/app_info/nexus_memory_purge_gate.h"

#include "base/command_line.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::CommandLine WithSwitch(bool present) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  if (present) {
    command_line.AppendSwitch(kMemoryPurgeForTestingSwitch);
  }
  return command_line;
}

TEST(NexusMemoryPurgeGateTest, ReleaseBuildAlwaysPurgesAndIgnoresTheSwitch) {
  EXPECT_TRUE(MemoryPurgeEnabled(true, WithSwitch(false)));
  EXPECT_TRUE(MemoryPurgeEnabled(true, WithSwitch(true)));
  EXPECT_FALSE(MemoryPurgeForTesting(true, WithSwitch(true)));
}

TEST(NexusMemoryPurgeGateTest, DebugBuildPurgesOnlyWithTheTestingSwitch) {
  EXPECT_FALSE(MemoryPurgeEnabled(false, WithSwitch(false)));
  EXPECT_FALSE(MemoryPurgeForTesting(false, WithSwitch(false)));
  EXPECT_TRUE(MemoryPurgeEnabled(false, WithSwitch(true)));
  EXPECT_TRUE(MemoryPurgeForTesting(false, WithSwitch(true)));
}

TEST(NexusMemoryPurgeGateTest, StartupGraceCoversTheFirstMinute) {
  const base::Time start = base::Time::FromSecondsSinceUnixEpoch(1'000'000);
  EXPECT_TRUE(InMemoryPurgeStartupGrace(start, start));
  EXPECT_TRUE(InMemoryPurgeStartupGrace(start, start + base::Seconds(59)));
  EXPECT_FALSE(InMemoryPurgeStartupGrace(start, start + base::Seconds(60)));
  EXPECT_FALSE(InMemoryPurgeStartupGrace(start, start + base::Hours(3)));
}

TEST(NexusMemoryPurgeGateTest, UnknownStartOrBackwardClockCountsAsInsideTheGrace) {
  const base::Time start = base::Time::FromSecondsSinceUnixEpoch(1'000'000);
  EXPECT_TRUE(InMemoryPurgeStartupGrace(base::Time(), start));
  EXPECT_TRUE(InMemoryPurgeStartupGrace(start, start - base::Seconds(5)));
}

TEST(NexusMemoryPurgeGateTest, TheTestingSeamOpensTheGateWhileAlive) {
  ScopedMemoryPurgeGateOpenForTesting open;
  EXPECT_TRUE(MemoryPurgeEnabledInThisProcess());
  EXPECT_FALSE(InMemoryPurgeStartupGraceNow());
}

}  // namespace
}  // namespace nexus
