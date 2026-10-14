// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/host_shell.h"

#include <optional>
#include <string>

#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

HostShellAvailability Shells(bool bash, bool pwsh_core, bool windows_powershell) {
  HostShellAvailability shells;
  shells.bash = bash;
  shells.pwsh_core = pwsh_core;
  shells.windows_powershell = windows_powershell;
  return shells;
}

#if BUILDFLAG(IS_WIN)
constexpr char kComspec[] = "C:\\WINDOWS\\system32\\cmd.exe";

// The regression: Velloc started from Explorer has no $SHELL, so the old
// report fell back to %COMSPEC% and the prompt read `Shell: cmd` while the
// Bash tool ran Git Bash — models then wrote `set VAR=value && cmd`, which
// bash does not export.
TEST(HostShellTest, WindowsReportsBashWheneverTheBashToolRuns) {
  EXPECT_EQ("bash", ChooseReportedShellName(Shells(true, false, true),
                                            std::nullopt, kComspec));
  // Bash wins over a $SHELL naming something else, and over PowerShell.
  EXPECT_EQ("bash", ChooseReportedShellName(Shells(true, true, true),
                                            std::string("/usr/bin/zsh"),
                                            kComspec));
}

TEST(HostShellTest, WindowsWithoutBashReportsThePowerShellTheToolLaunches) {
  EXPECT_EQ("pwsh", ChooseReportedShellName(Shells(false, true, true),
                                            std::nullopt, kComspec));
  EXPECT_EQ("powershell", ChooseReportedShellName(Shells(false, false, true),
                                                  std::nullopt, kComspec));
}

TEST(HostShellTest, WindowsWithNoToolShellFallsBackToShellThenComspec) {
  EXPECT_EQ("cmd", ChooseReportedShellName(Shells(false, false, false),
                                           std::nullopt, kComspec));
  EXPECT_EQ("cmd", ChooseReportedShellName(Shells(false, false, false),
                                           std::string(), kComspec));
  EXPECT_EQ("nu", ChooseReportedShellName(Shells(false, false, false),
                                          std::string("C:\\tools\\nu.exe"),
                                          kComspec));
  EXPECT_EQ("", ChooseReportedShellName(Shells(false, false, false),
                                        std::nullopt, std::nullopt));
}

TEST(HostShellTest, ExtractShellBasenameStripsPathAndExe) {
  EXPECT_EQ("bash",
            ExtractShellBasename(std::string("C:\\Program Files\\Git\\bin\\bash.exe")));
  EXPECT_EQ("cmd", ExtractShellBasename(std::string(kComspec)));
  // Only a trailing `.exe` goes; a name that is just ".exe" stays.
  EXPECT_EQ(".exe", ExtractShellBasename(std::string(".exe")));
}
#else
TEST(HostShellTest, PosixReportsShellEnvRegardlessOfToolShells) {
  EXPECT_EQ("zsh", ChooseReportedShellName(Shells(true, false, false),
                                           std::string("/usr/bin/zsh"),
                                           std::nullopt));
  EXPECT_EQ("", ChooseReportedShellName(Shells(true, false, false),
                                        std::nullopt, std::nullopt));
}

TEST(HostShellTest, ExtractShellBasenameStripsPath) {
  EXPECT_EQ("bash", ExtractShellBasename(std::string("/bin/bash")));
}
#endif

TEST(HostShellTest, ExtractShellBasenameEmptyInputIsUnknown) {
  EXPECT_EQ("", ExtractShellBasename(std::nullopt));
  EXPECT_EQ("", ExtractShellBasename(std::string()));
}

TEST(HostShellTest, PowershellMeansEitherPowerShell) {
  EXPECT_FALSE(Shells(true, false, false).powershell());
  EXPECT_TRUE(Shells(false, true, false).powershell());
  EXPECT_TRUE(Shells(false, false, true).powershell());
}

}  // namespace
}  // namespace nexus
