// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(NexusShellServiceLauncherTest, RetainedBackgroundShellsGateIdleRecycle) {
  base::test::SingleThreadTaskEnvironment task_environment;
  NexusShellServiceLauncher launcher;

  launcher.RetainBackgroundShell("bg_one");
  launcher.RetainBackgroundShell("bg_one");
  launcher.RetainBackgroundShell("bg_two");

  EXPECT_TRUE(launcher.IsBackgroundShellRetainedForTesting("bg_one"));
  EXPECT_TRUE(launcher.IsBackgroundShellRetainedForTesting("bg_two"));
  EXPECT_EQ(2u, launcher.retained_background_shell_count_for_testing());

  launcher.ReleaseBackgroundShell("bg_one");
  EXPECT_FALSE(launcher.IsBackgroundShellRetainedForTesting("bg_one"));
  EXPECT_TRUE(launcher.IsBackgroundShellRetainedForTesting("bg_two"));
  EXPECT_EQ(1u, launcher.retained_background_shell_count_for_testing());

  launcher.ReleaseBackgroundShell("bg_two");
  EXPECT_EQ(0u, launcher.retained_background_shell_count_for_testing());
}

}  // namespace
}  // namespace nexus
