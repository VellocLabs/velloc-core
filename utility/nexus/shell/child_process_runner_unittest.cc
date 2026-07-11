// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/child_process_runner.h"

#include <optional>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "custom_browser/utility/nexus/shell/safe_command_policy.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Returns an absolute path to a host shell binary that is essentially
// guaranteed to exist on the test machine. We avoid GetTestDataDir
// helpers because we want this test to run in any utility-process unit
// test target without extra runtime data.
base::FilePath FindTestShell() {
#if BUILDFLAG(IS_WIN)
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> sysroot = env->GetVar("SystemRoot");
  if (!sysroot.has_value() || sysroot->empty()) {
    return base::FilePath();
  }
  return base::FilePath::FromUTF8Unsafe(*sysroot)
      .AppendASCII("System32")
      .AppendASCII("cmd.exe");
#else
  return base::FilePath("/bin/sh");
#endif
}

// Builds an "echo hello" invocation appropriate for the host shell.
ChildProcessRequest MakeEchoRequest(const base::FilePath& cwd) {
  ChildProcessRequest req;
  req.exe = FindTestShell();
  req.cwd = cwd;
  req.timeout = base::Seconds(10);
  req.max_output_bytes = 1024;
#if BUILDFLAG(IS_WIN)
  req.args = {"/C", "echo hello"};
#else
  req.args = {"-c", "echo hello"};
#endif
  return req;
}

class ChildProcessRunnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(tmp_.CreateUniqueTempDir());
    if (FindTestShell().empty()) {
      GTEST_SKIP() << "Host has no usable test shell; cannot exercise runner";
    }
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir tmp_;
};

TEST_F(ChildProcessRunnerTest, RunsSimpleEchoCommand) {
  ChildProcessRequest req = MakeEchoRequest(tmp_.GetPath());
  const ChildProcessOutcome outcome = RunChildProcess(req);

  ASSERT_TRUE(outcome.launched) << outcome.launch_error_message;
  ASSERT_TRUE(outcome.code.has_value()) << "Should exit with a real code";
  EXPECT_EQ(*outcome.code, 0);
  EXPECT_FALSE(outcome.timed_out);
  EXPECT_FALSE(outcome.killed);
  EXPECT_FALSE(outcome.stdout_truncated);

  const std::string stdout_str(outcome.stdout_bytes.begin(),
                               outcome.stdout_bytes.end());
  EXPECT_NE(stdout_str.find("hello"), std::string::npos)
      << "Got stdout: " << stdout_str;
}

TEST_F(ChildProcessRunnerTest, KillsChildOnTimeout) {
  ChildProcessRequest req;
  req.exe = FindTestShell();
  req.cwd = tmp_.GetPath();
  // Tight cap so the test stays fast, but generous enough to avoid
  // racing the spawn itself on slow CI.
  req.timeout = base::Milliseconds(500);
  req.max_output_bytes = 1024;
  // The long-running command below is an EXTERNAL binary (ping / sleep), not a
  // shell builtin, so it must be resolvable via PATH. The Exec path clears the
  // child environment by default (`clear_environment = !inherit_environment`),
  // which strips PATH — the child would then fail to find ping/sleep and exit
  // immediately, never reaching the timeout. Inherit the parent environment so
  // the command actually runs long enough to be timeout-killed.
  req.inherit_environment = true;

#if BUILDFLAG(IS_WIN)
  // ping with -n 30 sleeps about 29 seconds — far past our 500ms cap.
  req.args = {"/C", "ping -n 30 127.0.0.1 > nul"};
#else
  req.args = {"-c", "sleep 30"};
#endif

  const base::TimeTicks start = base::TimeTicks::Now();
  const ChildProcessOutcome outcome = RunChildProcess(req);
  const base::TimeDelta elapsed = base::TimeTicks::Now() - start;

  ASSERT_TRUE(outcome.launched);
  EXPECT_TRUE(outcome.timed_out);
  EXPECT_TRUE(outcome.killed);
  EXPECT_LT(elapsed, base::Seconds(15))
      << "Timeout kill should be much faster than the script's natural "
         "runtime (30s)";
}

TEST_F(ChildProcessRunnerTest, TruncatesOutputAtCap) {
  ChildProcessRequest req;
  req.exe = FindTestShell();
  req.cwd = tmp_.GetPath();
  req.timeout = base::Seconds(15);
  req.max_output_bytes = 32;  // tiny cap

#if BUILDFLAG(IS_WIN)
  // for /L %i in (1,1,200) do @echo line%i > prints ~1500 chars
  req.args = {"/C", "for /L %i in (1,1,200) do @echo line%i"};
#else
  req.args = {"-c", "for i in $(seq 1 200); do echo line$i; done"};
#endif

  const ChildProcessOutcome outcome = RunChildProcess(req);
  ASSERT_TRUE(outcome.launched);
  EXPECT_TRUE(outcome.stdout_truncated)
      << "Cap was 32 bytes; got " << outcome.stdout_bytes.size() << " bytes";
  EXPECT_LE(outcome.stdout_bytes.size(), req.max_output_bytes);
}

TEST_F(ChildProcessRunnerTest, TruncatedOutputKeepsTail) {
  // Regression (claude_code `tailFile` parity): a capped read must keep the
  // END of the output — the verdict of a long build/test log — not the first
  // `max_output_bytes`. The prefix-keeping read this guards against made a
  // capped background build's shell.status view end right after its first
  // pages of output, hiding the actual result.
  ChildProcessRequest req;
  req.exe = FindTestShell();
  req.cwd = tmp_.GetPath();
  req.timeout = base::Seconds(15);
  req.max_output_bytes = 64;  // small enough that early lines cannot survive

#if BUILDFLAG(IS_WIN)
  req.args = {"/C", "for /L %i in (1,1,200) do @echo line%i"};
#else
  req.args = {"-c", "for i in $(seq 1 200); do echo line$i; done"};
#endif

  const ChildProcessOutcome outcome = RunChildProcess(req);
  ASSERT_TRUE(outcome.launched);
  EXPECT_TRUE(outcome.stdout_truncated);
  ASSERT_LE(outcome.stdout_bytes.size(), req.max_output_bytes);
  const std::string text(outcome.stdout_bytes.begin(),
                         outcome.stdout_bytes.end());
  // The final line survives the cap; the first line does not.
  EXPECT_NE(text.find("line200"), std::string::npos) << "tail lost: " << text;
  EXPECT_EQ(text.find("line1\n"), std::string::npos)
      << "prefix should have been dropped: " << text;
  EXPECT_EQ(text.find("line1\r"), std::string::npos)
      << "prefix should have been dropped: " << text;
}

}  // namespace
}  // namespace nexus
