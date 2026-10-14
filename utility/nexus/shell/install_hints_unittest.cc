// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/install_hints.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(InstallHintsTest, RipgrepHasPlatformSpecificCommands) {
  // The model surfaces this string verbatim, so the test asserts on the
  // exact package-manager keywords it must see — not the prose around
  // them. If we remove or rename a recipe in install_hints.cc we want
  // this test to fail loudly.
  const std::string win = InstallHintFor("rg", "windows");
  EXPECT_NE(win.find("winget install"), std::string::npos);
  EXPECT_NE(win.find("ripgrep"), std::string::npos);

  const std::string mac = InstallHintFor("rg", "macos");
  EXPECT_NE(mac.find("brew install ripgrep"), std::string::npos);

  const std::string linux = InstallHintFor("rg", "linux");
  EXPECT_NE(linux.find("apt-get install"), std::string::npos);
  EXPECT_NE(linux.find("dnf install"), std::string::npos);
  EXPECT_NE(linux.find("pacman"), std::string::npos);
}

TEST(InstallHintsTest, GitHasPlatformSpecificCommands) {
  EXPECT_NE(InstallHintFor("git", "windows").find("winget install"),
            std::string::npos);
  EXPECT_NE(InstallHintFor("git", "macos").find("brew install git"),
            std::string::npos);
  EXPECT_NE(InstallHintFor("git", "linux").find("apt-get install"),
            std::string::npos);
}

TEST(InstallHintsTest, PwshAndPowershellShareTheSameHint) {
  // The two ids alias to the same recipe — we don't want the model
  // chasing distinct hints depending on which one the resolver reported.
  EXPECT_EQ(InstallHintFor("pwsh", "windows"),
            InstallHintFor("powershell", "windows"));
  EXPECT_EQ(InstallHintFor("pwsh", "linux"),
            InstallHintFor("powershell", "linux"));
}

TEST(InstallHintsTest, BashOnWindowsSuggestsGitBashOrWsl) {
  const std::string hint = InstallHintFor("bash", "windows");
  EXPECT_NE(hint.find("Git Bash"), std::string::npos);
  EXPECT_NE(hint.find("wsl --install"), std::string::npos);
}

TEST(InstallHintsTest, UnknownToolReturnsEmptyString) {
  EXPECT_TRUE(InstallHintFor("definitely-not-a-tool", "linux").empty());
  EXPECT_TRUE(InstallHintFor("rg", "haiku").empty());
}

}  // namespace
}  // namespace nexus
