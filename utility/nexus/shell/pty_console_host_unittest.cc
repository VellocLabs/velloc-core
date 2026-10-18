// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/pty_console_host.h"

#include <string>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/test/scoped_command_line.h"
#include "build/build_config.h"
#include "custom_browser/common/custom_browser_switches.h"
#include "custom_browser/common/nexus_runtime_paths.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Writes the two bundled-host file names into `dir`; `dll_bytes` is what
// conpty.dll holds.
void WriteFakeBundle(const base::FilePath& dir, std::string_view dll_bytes) {
  ASSERT_TRUE(base::WriteFile(dir.AppendASCII("conpty.dll"), dll_bytes));
  ASSERT_TRUE(base::WriteFile(dir.AppendASCII("OpenConsole.exe"), "x"));
}

TEST(PtyConsoleHostTest, BundledPathNeedsBothFiles) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  EXPECT_TRUE(custom_browser::GetBundledConsoleHostPathIn(base::FilePath())
                  .empty());
  EXPECT_TRUE(
      custom_browser::GetBundledConsoleHostPathIn(tmp.GetPath()).empty());

  ASSERT_TRUE(base::WriteFile(tmp.GetPath().AppendASCII("conpty.dll"), "x"));
  EXPECT_TRUE(
      custom_browser::GetBundledConsoleHostPathIn(tmp.GetPath()).empty())
      << "conpty.dll without OpenConsole.exe is not a bundle";

  ASSERT_TRUE(base::WriteFile(tmp.GetPath().AppendASCII("OpenConsole.exe"),
                              "x"));
#if BUILDFLAG(IS_WIN)
  EXPECT_EQ(custom_browser::GetBundledConsoleHostPathIn(tmp.GetPath()),
            tmp.GetPath().AppendASCII("conpty.dll"));
#else
  EXPECT_TRUE(
      custom_browser::GetBundledConsoleHostPathIn(tmp.GetPath()).empty());
#endif
}

TEST(PtyConsoleHostTest, SwitchForcesTheSystemHost) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  WriteFakeBundle(tmp.GetPath(), "x");
#if BUILDFLAG(IS_WIN)
  EXPECT_FALSE(PtyConsoleHost::BundledDllForThisProcess(tmp.GetPath()).empty());
#endif

  base::test::ScopedCommandLine scoped_command_line;
  scoped_command_line.GetProcessCommandLine()->AppendSwitchASCII(
      custom_browser::switches::kConsoleHost,
      custom_browser::switches::kConsoleHostSystem);
  EXPECT_TRUE(PtyConsoleHost::BundledDllForThisProcess(tmp.GetPath()).empty());
}

TEST(PtyConsoleHostTest, TestOverrideReplacesTheTable) {
  PtyConsoleHostTable none;
  PtyConsoleHost::SetTableForTesting(none);
  EXPECT_FALSE(PtyConsoleHost::Get().available());
  EXPECT_TRUE(PtyConsoleHost::Get().host.empty());
  PtyConsoleHost::SetTableForTesting(std::nullopt);
}

#if BUILDFLAG(IS_WIN)

TEST(PtyConsoleHostTest, NoBundledDllResolvesTheSystemHost) {
  const PtyConsoleHostTable table = PtyConsoleHost::Resolve(base::FilePath());
  if (!table.available()) {
    GTEST_SKIP() << "kernel32 has no CreatePseudoConsole on this host";
  }
  EXPECT_EQ(table.host, "system");
  EXPECT_NE(table.create, nullptr);
  EXPECT_NE(table.resize, nullptr);
  EXPECT_NE(table.close, nullptr);
}

TEST(PtyConsoleHostTest, UnloadableBundledDllFallsBackToTheSystemHost) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  WriteFakeBundle(tmp.GetPath(), "this is not a PE image");
  const base::FilePath dll =
      PtyConsoleHost::BundledDllForThisProcess(tmp.GetPath());
  ASSERT_FALSE(dll.empty());

  const PtyConsoleHostTable table = PtyConsoleHost::Resolve(dll);
  const PtyConsoleHostTable system = PtyConsoleHost::Resolve(base::FilePath());
  EXPECT_EQ(table.available(), system.available());
  EXPECT_EQ(table.host, system.host);
}

TEST(PtyConsoleHostTest, BundledDllWithoutTheExportsFallsBack) {
  base::FilePath system_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &system_dir));
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  // A real, loadable DLL that exports no pseudoconsole functions.
  ASSERT_TRUE(base::CopyFile(system_dir.AppendASCII("version.dll"),
                             tmp.GetPath().AppendASCII("conpty.dll")));
  ASSERT_TRUE(base::WriteFile(tmp.GetPath().AppendASCII("OpenConsole.exe"),
                              "x"));
  const base::FilePath dll =
      PtyConsoleHost::BundledDllForThisProcess(tmp.GetPath());
  ASSERT_FALSE(dll.empty());

  const PtyConsoleHostTable table = PtyConsoleHost::Resolve(dll);
  const PtyConsoleHostTable system = PtyConsoleHost::Resolve(base::FilePath());
  EXPECT_EQ(table.available(), system.available());
  EXPECT_EQ(table.host, system.host);
}

TEST(PtyConsoleHostTest, RealBundledDllResolvesTheBundledHost) {
  const base::FilePath dll = custom_browser::GetBundledConsoleHostPath();
  if (dll.empty()) {
    GTEST_SKIP() << "no conpty.dll + OpenConsole.exe next to this binary";
  }
  const PtyConsoleHostTable table = PtyConsoleHost::Resolve(dll);
  EXPECT_TRUE(table.available());
  EXPECT_EQ(table.host, "bundled");
}

#endif  // BUILDFLAG(IS_WIN)

}  // namespace
}  // namespace nexus
