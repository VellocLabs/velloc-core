// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/pty_child_process.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/process/process_iterator.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/win/scoped_handle.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/common/nexus_runtime_paths.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"
#include "custom_browser/utility/nexus/shell/pty_console_host.h"
#include "custom_browser/utility/nexus/shell/safe_command_policy.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/cleanup/cleanup.h"

namespace nexus {
namespace {

constexpr base::TimeDelta kWait = base::Seconds(15);

// Drops the VT control sequences the console host interleaves with text, so
// a marker or a `mode con` number can be searched for as plain text.
std::string StripVt(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c != '\x1b') {
      if (c == '\r' || c == '\n' || c == '\t' ||
          static_cast<unsigned char>(c) >= 0x20) {
        out.push_back(c);
      }
      continue;
    }
    if (i + 1 >= in.size()) {
      break;
    }
    const char kind = in[++i];
    if (kind == '[') {
      // CSI: parameters and intermediates, then one final byte 0x40..0x7E.
      while (++i < in.size() &&
             !(static_cast<unsigned char>(in[i]) >= 0x40 &&
               static_cast<unsigned char>(in[i]) <= 0x7e)) {
      }
    } else if (kind == ']') {
      // OSC: up to BEL or ESC \.
      while (++i < in.size()) {
        if (in[i] == '\a') {
          break;
        }
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '\\') {
          ++i;
          break;
        }
      }
    }
    // Any other two-byte escape is consumed as a whole.
  }
  return out;
}

// The number after the last `label` (e.g. "Columns:") in `text` that is
// followed by digits, or nullopt.
std::optional<int> LastNumberAfter(const std::string& text,
                                   const std::string& label) {
  std::optional<int> value;
  size_t pos = 0;
  while ((pos = text.find(label, pos)) != std::string::npos) {
    size_t i = pos + label.size();
    while (i < text.size() && text[i] == ' ') {
      ++i;
    }
    size_t end = i;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
      ++end;
    }
    int parsed = 0;
    if (end > i && base::StringToInt(text.substr(i, end - i), &parsed)) {
      value = parsed;
    }
    pos = pos + label.size();
  }
  return value;
}

// True when some `mode con` report in `text` has both numbers.
bool HasModeConReport(const std::string& text, int cols, int rows) {
  size_t pos = 0;
  while ((pos = text.find("Columns:", pos)) != std::string::npos) {
    const size_t lines_pos = text.rfind("Lines:", pos);
    const std::string window =
        text.substr(lines_pos == std::string::npos ? pos : lines_pos,
                    pos + 40 - (lines_pos == std::string::npos ? pos : lines_pos));
    if (LastNumberAfter(window, "Columns:") == cols &&
        LastNumberAfter(window, "Lines:") == rows) {
      return true;
    }
    pos += 8;
  }
  return false;
}

std::set<base::ProcessId> PidsOf(const base::FilePath::StringType& exe) {
  std::set<base::ProcessId> pids;
  base::NamedProcessIterator it(exe, nullptr);
  while (const base::ProcessEntry* entry = it.NextProcessEntry()) {
    pids.insert(entry->pid());
  }
  return pids;
}

bool ProcessIsAlive(base::ProcessId pid) {
  base::Process process = base::Process::Open(pid);
  if (!process.IsValid()) {
    return false;
  }
  DWORD code = 0;
  return ::GetExitCodeProcess(process.Handle(), &code) && code == STILL_ACTIVE;
}

class RecordingClient : public mojom::NexusStreamingProcessClient {
 public:
  void OnStdout(const std::vector<uint8_t>& data) override {
    stdout_bytes.append(data.begin(), data.end());
  }
  void OnStderr(const std::vector<uint8_t>& data) override {
    stderr_calls++;
  }
  void OnExit(mojom::ShellExitInfoPtr exit) override { exited = std::move(exit); }

  std::string stdout_bytes;
  int stderr_calls = 0;
  mojom::ShellExitInfoPtr exited;
};

// One console host per run: "system" (kernel32) and, when the files are next
// to this binary, "bundled".
class PtyChildProcessTest : public testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(tmp_.CreateUniqueTempDir());
    const std::string host = GetParam();
    if (host == "bundled") {
      const base::FilePath dll = custom_browser::GetBundledConsoleHostPath();
      if (dll.empty()) {
        GTEST_SKIP() << "no conpty.dll + OpenConsole.exe next to this binary";
      }
      table_ = PtyConsoleHost::Resolve(dll);
    } else {
      table_ = PtyConsoleHost::Resolve(base::FilePath());
    }
    if (!table_.available()) {
      GTEST_SKIP() << "no pseudoconsole on this host";
    }
    ASSERT_EQ(table_.host, host);
    std::unique_ptr<base::Environment> env = base::Environment::Create();
    std::optional<std::string> comspec = env->GetVar("ComSpec");
    ASSERT_TRUE(comspec.has_value() && !comspec->empty());
    cmd_exe_ = base::FilePath::FromUTF8Unsafe(*comspec);
  }

  void TearDown() override {
    PtyChildProcess::SetLaunchFlagsForTesting(std::nullopt);
    if (child_) {
      child_.reset();
      RunFor(base::Milliseconds(200));
    }
  }

  void Spawn(std::vector<std::string> args, uint16_t cols, uint16_t rows) {
    ChildProcessRequest request;
    request.exe = cmd_exe_;
    request.args = std::move(args);
    request.cwd = tmp_.GetPath();
    request.env = SafeCommandPolicy::BuildFullEnvironment(
        {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}});
    client_ = std::make_unique<RecordingClient>();
    client_receiver_ =
        std::make_unique<mojo::Receiver<mojom::NexusStreamingProcessClient>>(
            client_.get());
    controller_.reset();
    closed_ = false;
    child_ = std::make_unique<PtyChildProcess>(
        std::move(request), cols, rows, table_,
        controller_.BindNewPipeAndPassReceiver(),
        client_receiver_->BindNewPipeAndPassRemote(),
        base::BindLambdaForTesting([this] { closed_ = true; }));
    child_->Start();
  }

  void Write(const std::string& text) {
    controller_->WriteStdin(std::vector<uint8_t>(text.begin(), text.end()));
  }

  std::string Text() const { return StripVt(client_->stdout_bytes); }

  void RunFor(base::TimeDelta delay) {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), delay);
    loop.Run();
  }

  // Polls `condition` every 20 ms until it holds or `timeout` passes.
  bool WaitUntil(base::RepeatingCallback<bool()> condition,
                 base::TimeDelta timeout = kWait) {
    const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
    while (!condition.Run()) {
      if (base::TimeTicks::Now() > deadline) {
        return false;
      }
      RunFor(base::Milliseconds(20));
    }
    return true;
  }

  bool WaitForText(const std::string& marker) {
    return WaitUntil(base::BindLambdaForTesting([&] {
      return Text().find(marker) != std::string::npos;
    }));
  }

  bool WaitForExit() {
    return WaitUntil(
        base::BindLambdaForTesting([&] { return !!client_->exited; }));
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir tmp_;
  PtyConsoleHostTable table_;
  base::FilePath cmd_exe_;
  std::unique_ptr<RecordingClient> client_;
  std::unique_ptr<mojo::Receiver<mojom::NexusStreamingProcessClient>>
      client_receiver_;
  mojo::Remote<mojom::NexusStreamingProcess> controller_;
  std::unique_ptr<PtyChildProcess> child_;
  bool closed_ = false;
};

TEST_P(PtyChildProcessTest, EchoesAMarkerAndExitsZero) {
  Spawn({"/c", "echo velloc-pty-marker"}, 80, 24);
  ASSERT_TRUE(WaitForExit()) << Text();
  EXPECT_NE(Text().find("velloc-pty-marker"), std::string::npos) << Text();
  ASSERT_TRUE(client_->exited->code.has_value());
  EXPECT_EQ(*client_->exited->code, 0);
  EXPECT_FALSE(client_->exited->killed);
  EXPECT_EQ(client_->stderr_calls, 0);
  EXPECT_TRUE(WaitUntil(base::BindLambdaForTesting([&] { return closed_; })));
}

TEST_P(PtyChildProcessTest, ModeConSeesTheRequestedSizeAndTheResize) {
  Spawn({}, 100, 30);
  Write("mode con\r");
  ASSERT_TRUE(WaitUntil(base::BindLambdaForTesting(
      [&] { return HasModeConReport(Text(), 100, 30); })))
      << Text();

  auto size = mojom::PtySize::New();
  size->cols = 120;
  size->rows = 40;
  controller_->Resize(std::move(size));
  Write("mode con\r");
  ASSERT_TRUE(WaitUntil(base::BindLambdaForTesting(
      [&] { return HasModeConReport(Text(), 120, 40); })))
      << Text();

  Write("exit\r");
  ASSERT_TRUE(WaitForExit()) << Text();
  EXPECT_EQ(client_->exited->code.value_or(-1), 0);
}

TEST_P(PtyChildProcessTest, ExitTypedIntoCmdEndsItWithZero) {
  Spawn({}, 80, 24);
  Write("exit\r");
  ASSERT_TRUE(WaitForExit()) << Text();
  ASSERT_TRUE(client_->exited->code.has_value());
  EXPECT_EQ(*client_->exited->code, 0);
  EXPECT_FALSE(client_->exited->killed);
}

TEST_P(PtyChildProcessTest, KillDuringContinuousOutputEndsPromptly) {
  Spawn({"/c", "for /l %i in (1,0,2) do @echo velloc-spin"}, 80, 24);
  ASSERT_TRUE(WaitForText("velloc-spin")) << Text();
  const base::TimeTicks kill_at = base::TimeTicks::Now();
  controller_->Kill();
  ASSERT_TRUE(WaitUntil(
      base::BindLambdaForTesting([&] { return !!client_->exited && closed_; }),
      base::Seconds(10)));
  EXPECT_LT(base::TimeTicks::Now() - kill_at, base::Seconds(5));
  EXPECT_TRUE(client_->exited->killed);
  EXPECT_FALSE(client_->exited->code.has_value());
}

TEST_P(PtyChildProcessTest, GrandchildDiesWithTheTree) {
  const base::FilePath::StringType kWaitFor = FILE_PATH_LITERAL("waitfor.exe");
  const std::set<base::ProcessId> before = PidsOf(kWaitFor);
  Spawn({}, 80, 24);
  Write(base::StringPrintf("start /b waitfor VellocPty%u /t 300\r",
                           static_cast<unsigned>(::GetCurrentProcessId())));
  base::ProcessId grandchild = 0;
  ASSERT_TRUE(WaitUntil(base::BindLambdaForTesting([&] {
    for (base::ProcessId pid : PidsOf(kWaitFor)) {
      if (!before.count(pid)) {
        grandchild = pid;
        return true;
      }
    }
    return false;
  }))) << Text();

  controller_->Kill();
  ASSERT_TRUE(WaitForExit());
  EXPECT_TRUE(WaitUntil(base::BindLambdaForTesting(
      [&] { return !ProcessIsAlive(grandchild); })))
      << "waitfor.exe " << grandchild << " outlived the killed terminal";
}

// The child is in the terminal's job before any of its code has run. (This
// test process may itself sit in a job, so the check names the terminal's
// job.)
TEST_P(PtyChildProcessTest, ChildIsInTheJobWhenCreated) {
  std::optional<bool> in_job;
  base::Lock lock;
  PtyChildProcess::LaunchFlags flags;
  flags.on_created =
      base::BindLambdaForTesting([&](HANDLE process, HANDLE job) {
        BOOL result = FALSE;
        const bool ok = ::IsProcessInJob(process, job, &result);
        base::AutoLock guard(lock);
        in_job = ok && result;
      });
  PtyChildProcess::SetLaunchFlagsForTesting(flags);

  Spawn({"/c", "echo velloc-job"}, 80, 24);
  ASSERT_TRUE(WaitForExit()) << Text();
  base::AutoLock guard(lock);
  ASSERT_TRUE(in_job.has_value());
  EXPECT_TRUE(*in_job);
}

// The shell's very first act starts a grandchild; Kill still takes it.
TEST_P(PtyChildProcessTest, GrandchildStartedAtOnceDiesWithTheTree) {
  const base::FilePath::StringType kWaitFor = FILE_PATH_LITERAL("waitfor.exe");
  const std::set<base::ProcessId> before = PidsOf(kWaitFor);
  const unsigned self = static_cast<unsigned>(::GetCurrentProcessId());
  Spawn({"/c", base::StringPrintf("start /b waitfor VellocPtyFirst%u /t "
                                  "300 & waitfor VellocPtyHold%u /t 300",
                                  self, self)},
        80, 24);
  std::set<base::ProcessId> spawned;
  ASSERT_TRUE(WaitUntil(base::BindLambdaForTesting([&] {
    spawned.clear();
    for (base::ProcessId pid : PidsOf(kWaitFor)) {
      if (!before.count(pid)) {
        spawned.insert(pid);
      }
    }
    return spawned.size() >= 2;
  }))) << Text();

  controller_->Kill();
  ASSERT_TRUE(WaitForExit());
  for (base::ProcessId pid : spawned) {
    EXPECT_TRUE(WaitUntil(
        base::BindLambdaForTesting([&] { return !ProcessIsAlive(pid); })))
        << "waitfor.exe " << pid << " outlived the killed terminal";
  }
}

// This process's stdout and stderr are redirected to a file, the way
// `--enable-logging` redirects the utility process's; the child's output
// must still come through the pseudoconsole and never reach that file.
TEST_P(PtyChildProcessTest, RedirectedStdioOfThisProcessStaysOutOfTheChild) {
  const base::FilePath capture = tmp_.GetPath().AppendASCII("redirect.txt");
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  base::win::ScopedHandle file(::CreateFileW(
      capture.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(file.is_valid());
  const HANDLE old_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  const HANDLE old_err = ::GetStdHandle(STD_ERROR_HANDLE);
  ASSERT_TRUE(::SetStdHandle(STD_OUTPUT_HANDLE, file.get()));
  ASSERT_TRUE(::SetStdHandle(STD_ERROR_HANDLE, file.get()));
  absl::Cleanup restore = [&] {
    ::SetStdHandle(STD_OUTPUT_HANDLE, old_out);
    ::SetStdHandle(STD_ERROR_HANDLE, old_err);
  };

  Spawn({"/c", "echo velloc-redir-out & echo velloc-redir-err 1>&2"}, 80, 24);
  ASSERT_TRUE(WaitForExit()) << Text();
  EXPECT_NE(Text().find("velloc-redir-out"), std::string::npos) << Text();
  EXPECT_NE(Text().find("velloc-redir-err"), std::string::npos) << Text();

  ::FlushFileBuffers(file.get());
  std::string captured;
  ASSERT_TRUE(base::ReadFileToString(capture, &captured));
  EXPECT_EQ(captured.find("velloc-redir"), std::string::npos) << captured;
}

// Negative control for the test above: without STARTF_USESTDHANDLES the
// child keeps this process's std handles and its output bypasses the
// console. Records what happens so the production flags are a proven
// choice, not a guess.
TEST_P(PtyChildProcessTest, RedirectedStdioLeaksWithoutUnsetStdHandles) {
  PtyChildProcess::LaunchFlags flags;
  flags.unset_std_handles = false;
  PtyChildProcess::SetLaunchFlagsForTesting(flags);

  const base::FilePath capture = tmp_.GetPath().AppendASCII("redirect.txt");
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  base::win::ScopedHandle file(::CreateFileW(
      capture.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(file.is_valid());
  const HANDLE old_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  const HANDLE old_err = ::GetStdHandle(STD_ERROR_HANDLE);
  ASSERT_TRUE(::SetStdHandle(STD_OUTPUT_HANDLE, file.get()));
  ASSERT_TRUE(::SetStdHandle(STD_ERROR_HANDLE, file.get()));
  absl::Cleanup restore = [&] {
    ::SetStdHandle(STD_OUTPUT_HANDLE, old_out);
    ::SetStdHandle(STD_ERROR_HANDLE, old_err);
  };

  Spawn({"/c", "echo velloc-leak-out & echo velloc-leak-err 1>&2"}, 80, 24);
  ASSERT_TRUE(WaitForExit()) << Text();
  ::FlushFileBuffers(file.get());
  std::string captured;
  ASSERT_TRUE(base::ReadFileToString(capture, &captured));
  const bool through_console =
      Text().find("velloc-leak-out") != std::string::npos;
  const bool into_file = captured.find("velloc-leak") != std::string::npos;
  RecordProperty("through_console", through_console ? "yes" : "no");
  RecordProperty("into_file", into_file ? "yes" : "no");
  EXPECT_FALSE(through_console)
      << "the std-handle flag is not what keeps redirected stdio out";
}

INSTANTIATE_TEST_SUITE_P(Hosts,
                         PtyChildProcessTest,
                         testing::Values("system", "bundled"),
                         [](const testing::TestParamInfo<const char*>& info) {
                           return std::string(info.param);
                         });

}  // namespace
}  // namespace nexus
