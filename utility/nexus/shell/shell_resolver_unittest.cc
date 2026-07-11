// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/shell_resolver.h"

#include <algorithm>
#include <string>

#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(ShellResolverTest, KnownToolIdsIsAlphabetisedAndDeduped) {
  const std::vector<std::string> ids = ShellResolver::KnownToolIds();
  ASSERT_FALSE(ids.empty());
  std::vector<std::string> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted, ids) << "KnownToolIds() must be alphabetised";

  std::vector<std::string> unique = ids;
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  EXPECT_EQ(unique.size(), ids.size())
      << "KnownToolIds() must not contain duplicates";
}

TEST(ShellResolverTest, ResolveReportsHostPlatform) {
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();

#if BUILDFLAG(IS_WIN)
  EXPECT_EQ(caps.platform, "windows");
#elif BUILDFLAG(IS_MAC)
  EXPECT_EQ(caps.platform, "macos");
#elif BUILDFLAG(IS_LINUX)
  EXPECT_EQ(caps.platform, "linux");
#endif
}

TEST(ShellResolverTest, ResolveReturnsEntryForEveryKnownTool) {
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();
  const std::vector<std::string> known = ShellResolver::KnownToolIds();
  ASSERT_EQ(caps.tools.size(), known.size())
      << "Every probed tool must appear in `tools`, available or not";

  for (size_t i = 0; i < known.size(); ++i) {
    EXPECT_EQ(caps.tools[i].id, known[i]);
    if (!caps.tools[i].available) {
      // Per the contract: when not available, path / version are empty.
      EXPECT_TRUE(caps.tools[i].path.empty());
      EXPECT_TRUE(caps.tools[i].version.empty());
    } else {
      EXPECT_FALSE(caps.tools[i].path.empty())
          << "Available tool '" << caps.tools[i].id << "' must have a path";
    }
  }
}

TEST(ShellResolverTest, CommandAllowlistMirrorsAvailableTools) {
  // Slice 2 contract: every tool reported as available appears in the
  // allowlist exactly once, in KnownToolIds() order; tools that are
  // unavailable are *not* in the allowlist.
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();

  std::vector<std::string> expected;
  for (const ResolvedTool& tool : caps.tools) {
    if (tool.available) {
      expected.push_back(tool.id);
    }
  }
  EXPECT_EQ(caps.command_allowlist, expected);

  for (const std::string& id : caps.command_allowlist) {
    const auto it = std::ranges::find_if(
        caps.tools, [&](const ResolvedTool& t) { return t.id == id; });
    ASSERT_NE(it, caps.tools.end());
    EXPECT_TRUE(it->available)
        << "Allowlist entry '" << id << "' must be marked available";
  }
}

TEST(ShellResolverTest, DefaultShellIsOneOfTheKnownIds) {
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();
  if (!caps.default_shell.has_value()) {
    GTEST_SKIP() << "Host has no usable shell on PATH; nothing to verify";
  }

  const std::vector<std::string> known = ShellResolver::KnownToolIds();
  EXPECT_NE(std::ranges::find(known, caps.default_shell->id), known.end());
  EXPECT_TRUE(caps.default_shell->available);
  EXPECT_FALSE(caps.default_shell->path.empty());
}

}  // namespace
}  // namespace nexus
