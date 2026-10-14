// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/ui/view/relaunch_command_line.h"

#include <string_view>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser::window_util {
namespace {

// The window-kind switches window_util excludes in production. Spelled out
// here (not pulled from chrome_switches) so the test stays a //base-only leaf.
constexpr std::string_view kWindowKindSwitches[] = {
    "app", "app-id", "incognito", "guest", "notification-launch-id"};

base::CommandLine MakeCommandLine(
    std::initializer_list<base::CommandLine::StringViewType> argv) {
  base::CommandLine::StringVector args;
  args.emplace_back(FILE_PATH_LITERAL("chrome.exe"));
  for (const auto& arg : argv) {
    args.emplace_back(arg);
  }
  return base::CommandLine(args);
}

TEST(RelaunchCommandLineTest, BareExeIsPlainRelaunch) {
  const base::CommandLine cmd = MakeCommandLine({});
  EXPECT_FALSE(HasExternalUrlArgs(cmd));
  EXPECT_TRUE(IsPlainRelaunchCommandLine(cmd, kWindowKindSwitches));
}

TEST(RelaunchCommandLineTest, ProfileSwitchAloneIsPlainRelaunch) {
  // The desktop shortcut of a secondary profile carries --profile-directory;
  // that names WHO, not WHAT to open.
  const base::CommandLine cmd =
      MakeCommandLine({FILE_PATH_LITERAL("--profile-directory=Profile 1")});
  EXPECT_TRUE(IsPlainRelaunchCommandLine(cmd, kWindowKindSwitches));
}

TEST(RelaunchCommandLineTest, AboutBlankPlaceholderIsExempt) {
  // The e2e harness appends `about:blank` to every launch (see
  // launchBrowser.ts); it is not an external open.
  const base::CommandLine cmd =
      MakeCommandLine({FILE_PATH_LITERAL("about:blank")});
  EXPECT_FALSE(HasExternalUrlArgs(cmd));
  EXPECT_TRUE(IsPlainRelaunchCommandLine(cmd, kWindowKindSwitches));
}

TEST(RelaunchCommandLineTest, UrlArgIsNotPlainRelaunch) {
  const base::CommandLine cmd =
      MakeCommandLine({FILE_PATH_LITERAL("https://example.com/")});
  EXPECT_TRUE(HasExternalUrlArgs(cmd));
  EXPECT_FALSE(IsPlainRelaunchCommandLine(cmd, kWindowKindSwitches));
}

TEST(RelaunchCommandLineTest, UrlArgNextToPlaceholderIsNotPlainRelaunch) {
  const base::CommandLine cmd =
      MakeCommandLine({FILE_PATH_LITERAL("about:blank"),
                       FILE_PATH_LITERAL("https://example.com/")});
  EXPECT_TRUE(HasExternalUrlArgs(cmd));
  EXPECT_FALSE(IsPlainRelaunchCommandLine(cmd, kWindowKindSwitches));
}

TEST(RelaunchCommandLineTest, WindowKindSwitchIsNotPlainRelaunch) {
  EXPECT_FALSE(IsPlainRelaunchCommandLine(
      MakeCommandLine({FILE_PATH_LITERAL("--app=https://example.com/")}),
      kWindowKindSwitches));
  EXPECT_FALSE(IsPlainRelaunchCommandLine(
      MakeCommandLine({FILE_PATH_LITERAL("--app-id=abc")}),
      kWindowKindSwitches));
  EXPECT_FALSE(IsPlainRelaunchCommandLine(
      MakeCommandLine({FILE_PATH_LITERAL("--incognito")}),
      kWindowKindSwitches));
  // Launches that carry their own action (a toast click, a guest window) are
  // dispatched by Chromium, never treated as "just open the product".
  EXPECT_FALSE(IsPlainRelaunchCommandLine(
      MakeCommandLine({FILE_PATH_LITERAL("--notification-launch-id=0|x")}),
      kWindowKindSwitches));
  EXPECT_FALSE(IsPlainRelaunchCommandLine(
      MakeCommandLine({FILE_PATH_LITERAL("--guest")}), kWindowKindSwitches));
}

TEST(RelaunchCommandLineTest, UnlistedSwitchDoesNotDemote) {
  // Only the caller's list counts: an unrelated switch (e.g. a logging flag)
  // keeps the launch a plain relaunch.
  const base::CommandLine cmd =
      MakeCommandLine({FILE_PATH_LITERAL("--enable-logging")});
  EXPECT_TRUE(IsPlainRelaunchCommandLine(cmd, kWindowKindSwitches));
}

}  // namespace
}  // namespace custom_browser::window_util
