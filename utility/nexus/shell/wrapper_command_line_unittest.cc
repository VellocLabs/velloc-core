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

TEST(WrapperCommandLineTest, AcceptsOnlyArgumentsFromTheSafeSet) {
  const base::FilePath wrapper(FILE_PATH_LITERAL("C:\\bin\\git.cmd"));
  for (const char* arg :
       {"rev-parse", "--verify", "--end-of-options", "HEAD~0",
        "HEAD@{upstream}~0", "refs/heads/feature/a.b_c-d",
        "0123456789abcdef0123456789abcdef01234567", "C:\\Users\\me\\repo",
        "C:\\dir\\", "src/a+b.ts", "@scope/pkg", "-y", "x:y"}) {
    EXPECT_FALSE(FindWrapperArgProblem(wrapper, {arg}).has_value()) << arg;
  }
}

TEST(WrapperCommandLineTest, RefusesEveryArgumentOutsideTheSafeSet) {
  const base::FilePath wrapper(FILE_PATH_LITERAL("C:\\bin\\npx.cmd"));
  // `a==a md x` runs `md x` through `if %~1 == help ...`; it needs only a
  // space or `=`.
  const struct {
    const char* arg;
    const char* what;
  } kRefused[] = {
      {"a==a md x", "an equals sign"},
      {"a==a", "an equals sign"},
      {"a b", "a space"},
      {"--format=%H", "an equals sign"},
      {"x;y", "a separator (, or ;)"},
      {"x,y", "a separator (, or ;)"},
      {"a&b", "an ampersand"},
      {"a|b", "a vertical bar"},
      {"<a", "a redirection sign"},
      {">a", "a redirection sign"},
      {"a%b", "a percent sign"},
      {"a!b", "an exclamation mark"},
      {"HEAD^{commit}", "a caret"},
      {"a\"b", "a double quote"},
      {"a\tb", "a control character"},
      {"a\nb", "a control character"},
      {"a\x7f" "b", "a control character"},
      {"", "nothing (an empty argument)"},
  };
  for (const auto& refused : kRefused) {
    const std::optional<WrapperArgProblem> problem =
        FindWrapperArgProblem(wrapper, {"ok", refused.arg});
    ASSERT_TRUE(problem.has_value()) << refused.arg;
    EXPECT_EQ(problem->index, 1u) << refused.arg;
    EXPECT_STREQ(problem->what, refused.what) << refused.arg;
  }
  for (const char* arg : {"(a)", "a?b", "a#b", "a'b", "a`b", "a*", "a[b]",
                          "caf\xc3\xa9", "\xef\xbc\x82"}) {
    EXPECT_TRUE(FindWrapperArgProblem(wrapper, {arg}).has_value()) << arg;
  }
  // The wrapper path is the host's: an operator in a directory name is kept
  // literal by the quotes and is not a caller's to exploit.
  EXPECT_FALSE(FindWrapperArgProblem(
                   base::FilePath(FILE_PATH_LITERAL("C:\\a&b (x86)\\npx.cmd")),
                   {"ok"})
                   .has_value());
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
                               "HEAD@{upstream}~0", "C:\\dir\\"}),
      L"\"C:\\Windows\\System32\\cmd.exe\" /D /V:OFF /S /C "
      L"\"\"C:\\Program Files\\nodejs\\npx.cmd\" -y @scope/server "
      L"C:\\Users\\me\\repo HEAD@{upstream}~0 C:\\dir\\\"");
}

}  // namespace
}  // namespace nexus
