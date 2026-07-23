// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
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

// The event-driven exit path: the utility fires `OnExit` once per background
// shell, and the launcher must broadcast it to every registered bridge sink
// (each guest filters by shellId on the JS side). Removing a sink stops its
// delivery; a null exit is ignored.
TEST(NexusShellServiceLauncherTest, BroadcastsShellExitToEventSinks) {
  base::test::SingleThreadTaskEnvironment task_environment;
  NexusShellServiceLauncher launcher;

  std::vector<std::pair<std::string, int>> sink_a;
  std::vector<std::pair<std::string, int>> sink_b;
  const uint64_t id_a = launcher.AddShellEventSink(base::BindRepeating(
      [](std::vector<std::pair<std::string, int>>* out,
         const std::string& shell_id, const mojom::ShellExitInfo& exit) {
        out->emplace_back(shell_id, exit.code.value_or(-1));
      },
      &sink_a));
  launcher.AddShellEventSink(base::BindRepeating(
      [](std::vector<std::pair<std::string, int>>* out,
         const std::string& shell_id, const mojom::ShellExitInfo& exit) {
        out->emplace_back(shell_id, exit.code.value_or(-1));
      },
      &sink_b));

  auto exit = mojom::ShellExitInfo::New();
  exit->code = 0;
  exit->timed_out = false;
  exit->killed = false;
  exit->duration_ms = 42;
  launcher.OnExit("bg_one", exit.Clone());

  // Both sinks saw the exit.
  ASSERT_EQ(1u, sink_a.size());
  EXPECT_EQ("bg_one", sink_a[0].first);
  EXPECT_EQ(0, sink_a[0].second);
  ASSERT_EQ(1u, sink_b.size());
  EXPECT_EQ("bg_one", sink_b[0].first);

  // Removing sink A stops its delivery; sink B still receives.
  launcher.RemoveShellEventSink(id_a);
  auto exit2 = mojom::ShellExitInfo::New();
  exit2->code = 3;
  exit2->timed_out = false;
  exit2->killed = false;
  exit2->duration_ms = 7;
  launcher.OnExit("bg_two", std::move(exit2));

  EXPECT_EQ(1u, sink_a.size());  // unchanged
  ASSERT_EQ(2u, sink_b.size());
  EXPECT_EQ("bg_two", sink_b[1].first);
  EXPECT_EQ(3, sink_b[1].second);
}

}  // namespace
}  // namespace nexus
