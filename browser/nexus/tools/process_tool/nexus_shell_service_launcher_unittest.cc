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

TEST(NexusShellServiceLauncherTest, CachesTerminalBackgroundOutput) {
  base::test::SingleThreadTaskEnvironment task_environment;
  NexusShellServiceLauncher launcher;

  NexusShellServiceLauncher::CachedBackgroundOutput output;
  output.exit_code = 7;
  output.timed_out = true;
  output.killed = true;
  output.duration_ms = 123;
  output.stdout_text = "out";
  output.stderr_text = "err";
  output.stdout_truncated = true;

  launcher.CacheBackgroundOutput("bg_done", output);

  std::optional<NexusShellServiceLauncher::CachedBackgroundOutput> cached =
      launcher.GetCachedBackgroundOutput("bg_done");
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(7, cached->exit_code);
  EXPECT_TRUE(cached->timed_out);
  EXPECT_TRUE(cached->killed);
  EXPECT_EQ(123u, cached->duration_ms);
  EXPECT_EQ("out", cached->stdout_text);
  EXPECT_EQ("err", cached->stderr_text);
  EXPECT_TRUE(cached->stdout_truncated);
  EXPECT_FALSE(cached->stderr_truncated);
  EXPECT_EQ(1u, launcher.cached_background_output_count_for_testing());
  EXPECT_FALSE(launcher.GetCachedBackgroundOutput("bg_missing").has_value());
}

}  // namespace
}  // namespace nexus
