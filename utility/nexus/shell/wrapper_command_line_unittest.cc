// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/wrapper_command_line.h"

#include "base/files/file_path.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(WrapperCommandLineTest, RecognisesWrappersInAnyCaseAndTrailingDots) {
  for (const auto* name :
       {FILE_PATH_LITERAL("npx.cmd"), FILE_PATH_LITERAL("git.BAT"),
        FILE_PATH_LITERAL("x.bat."), FILE_PATH_LITERAL("x.cmd "),
        FILE_PATH_LITERAL("x.CMD. .")}) {
    EXPECT_TRUE(IsBatchWrapper(base::FilePath(name))) << name;
  }
  for (const auto* name :
       {FILE_PATH_LITERAL("git.exe"), FILE_PATH_LITERAL("run.ps1"),
        FILE_PATH_LITERAL("cmd"), FILE_PATH_LITERAL("x.cmdx")}) {
    EXPECT_FALSE(IsBatchWrapper(base::FilePath(name))) << name;
  }
}

TEST(WrapperCommandLineTest, RefusesEachCharacterCmdChangesInsideQuotes) {
  const base::FilePath wrapper(FILE_PATH_LITERAL("C:\\bin\\npx.cmd"));
  for (const char* arg : {"a%b", "a!b", "a^b", "a\"b", "a\nb", "a\rb",
                          "a\tb", "a\x1b" "b", "a\x7f" "b"}) {
    const std::optional<WrapperArgProblem> problem =
        FindWrapperArgProblem(wrapper, {"ok", arg});
    ASSERT_TRUE(problem.has_value()) << arg;
    EXPECT_EQ(problem->index, 1u) << arg;
  }
  for (const char* arg : {"a&b", "a|b", "<a", ">a", "(a)", "a,b", "a;b",
                          "a=b", "a b", "", "C:\\x\\", "@s/p", "a'b", "a`b"}) {
    EXPECT_FALSE(FindWrapperArgProblem(wrapper, {arg}).has_value()) << arg;
  }
  const std::optional<WrapperArgProblem> in_path = FindWrapperArgProblem(
      base::FilePath(FILE_PATH_LITERAL("C:\\100%\\npx.cmd")), {"ok"});
  ASSERT_TRUE(in_path.has_value());
  EXPECT_FALSE(in_path->index.has_value());
}

TEST(WrapperCommandLineTest, QuotesTheLineForCmdWithSlashS) {
  const base::FilePath cmd(FILE_PATH_LITERAL("C:\\Windows\\System32\\cmd.exe"));
  const base::FilePath wrapper(
      FILE_PATH_LITERAL("C:\\Program Files\\nodejs\\npx.cmd"));
  EXPECT_EQ(BuildWrapperCommandLine(cmd, wrapper, {}),
            L"\"C:\\Windows\\System32\\cmd.exe\" /D /V:OFF /S /C "
            L"\"\"C:\\Program Files\\nodejs\\npx.cmd\"\"");
  EXPECT_EQ(
      BuildWrapperCommandLine(cmd, wrapper,
                              {"-y", "@scope/server", "C:\\Users\\me\\repo",
                               "a b", "", "x),else,(echo", "C:\\dir\\"}),
      L"\"C:\\Windows\\System32\\cmd.exe\" /D /V:OFF /S /C "
      L"\"\"C:\\Program Files\\nodejs\\npx.cmd\" -y @scope/server "
      L"C:\\Users\\me\\repo \"a b\" \"\" \"x),else,(echo\" C:\\dir\\\"");
  EXPECT_EQ(BuildWrapperCommandLine(cmd, wrapper, {"a b\\"}),
            L"\"C:\\Windows\\System32\\cmd.exe\" /D /V:OFF /S /C "
            L"\"\"C:\\Program Files\\nodejs\\npx.cmd\" \"a b\\\\\"\"");
}

}  // namespace
}  // namespace nexus
