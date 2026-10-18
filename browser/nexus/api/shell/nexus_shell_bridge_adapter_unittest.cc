// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/nexus_shell_bridge_adapter.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "base/environment.h"
#include "base/functional/callback.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/process.h"
#include "base/process/process_iterator.h"
#include "base/strings/stringprintf.h"
#include "custom_browser/utility/nexus/shell/nexus_shell_service_impl.h"
#include "custom_browser/utility/nexus/shell/pty_console_host.h"
#endif

namespace nexus {
namespace {

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int error_count = 0;
  base::Value last_success;
  std::string last_error_code;
  std::string last_error_message;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*result_binary_field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& message) override {
    ++error_count;
    last_error_code = code;
    last_error_message = message;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue ShellRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "shell");
  route.Set("action", std::string(action));
  return route;
}

// A `spawn` payload for `cmd /c <command>` under a `cols` x `rows`
// pseudoconsole.
base::DictValue PtySpawnPayload(const std::string& command,
                                int cols,
                                int rows) {
  base::DictValue payload;
  payload.Set("file", "cmd");
  base::ListValue args;
  args.Append("/c");
  args.Append(command);
  payload.Set("args", std::move(args));
  base::DictValue pty;
  pty.Set("cols", cols);
  pty.Set("rows", rows);
  payload.Set("pty", std::move(pty));
  return payload;
}

class NexusShellBridgeAdapterTest : public testing::Test {
 protected:
  // No profile: everything below is answered before the launcher is needed.
  void Dispatch(std::string_view action, base::DictValue payload) {
    EXPECT_TRUE(
        adapter_.Dispatch(action, "req", ShellRoute(action), payload, {}));
    task_environment_.RunUntilIdle();
  }

  // Pumps only this thread afterwards: a live console child keeps a reader
  // task on the pool, so waiting for the pool to go idle would never return.
  void DispatchTo(NexusShellBridgeAdapter& adapter,
                  std::string_view action,
                  base::DictValue payload) {
    EXPECT_TRUE(
        adapter.Dispatch(action, "req", ShellRoute(action), payload, {}));
    base::RunLoop().RunUntilIdle();
  }

  // Pumps the loop (real time) until `condition` holds or `timeout` passes.
  bool WaitUntil(base::RepeatingCallback<bool()> condition,
                 base::TimeDelta timeout = base::Seconds(30)) {
    const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
    while (!condition.Run()) {
      if (base::TimeTicks::Now() >= deadline) {
        return false;
      }
      base::RunLoop wait;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, wait.QuitClosure(), base::Milliseconds(50));
      wait.Run();
    }
    return true;
  }

  content::BrowserTaskEnvironment task_environment_;
  RecordingDelegate delegate_;
  NexusShellBridgeAdapter adapter_{&delegate_, nullptr};
};

TEST_F(NexusShellBridgeAdapterTest, SpawnTakesExactlyOneForm) {
  base::DictValue both;
  both.Set("shell", "bash");
  both.Set("command", "echo hi");
  both.Set("file", "node");
  Dispatch("spawn", std::move(both));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  Dispatch("spawn", base::DictValue());
  EXPECT_EQ(delegate_.error_count, 2);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue empty_file;
  empty_file.Set("file", "");
  Dispatch("spawn", std::move(empty_file));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

TEST_F(NexusShellBridgeAdapterTest, PersistNeedsAnAbsoluteLogPath) {
  base::DictValue payload;
  payload.Set("file", "node");
  base::ListValue args;
  args.Append("server.js");
  payload.Set("args", std::move(args));
  base::DictValue persist;
  persist.Set("logPath", "relative/out.log");
  payload.Set("persist", std::move(persist));
  Dispatch("spawn", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_NE(delegate_.last_error_message.find("logPath"), std::string::npos);
}

// A well-formed argv spawn with no profile reaches the launch step, which is
// where SHELL_UNAVAILABLE is decided — i.e. the form was accepted.
TEST_F(NexusShellBridgeAdapterTest, AWellFormedArgvSpawnPassesValidation) {
  base::DictValue payload;
  payload.Set("file", "node");
  payload.Set("args", base::ListValue());
  Dispatch("spawn", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "SHELL_UNAVAILABLE");
}

// `{{secret:NAME}}` in an env value is resolved before the launch, and a
// secret that cannot be resolved refuses the spawn instead of passing the
// placeholder text to the child. (No profile here, so no secret resolves; the
// resolving and the processEnv rule are NexusSecretStoreTest's.)
TEST_F(NexusShellBridgeAdapterTest, EnvSecretPlaceholdersResolveOrRefuse) {
  auto spawn_with_env = [&](const char* value) {
    base::DictValue payload;
    payload.Set("file", "node");
    payload.Set("args", base::ListValue());
    base::DictValue env;
    env.Set("API_TOKEN", value);
    payload.Set("env", std::move(env));
    Dispatch("spawn", std::move(payload));
  };
  spawn_with_env("{{secret:mcp:server:API_TOKEN}}");
  EXPECT_EQ(delegate_.last_error_code, "SECRET_UNAVAILABLE");
  spawn_with_env("{{secret:mcp:unterminated");
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  // A plain value is not a placeholder: the spawn reaches the launch step.
  spawn_with_env("plain");
  EXPECT_EQ(delegate_.last_error_code, "SHELL_UNAVAILABLE");
}

// A pty child is interactive: the one-shot stdin inputs and `persist` make
// no sense with it and are refused before anything is launched.
TEST_F(NexusShellBridgeAdapterTest, PtyRefusesPersistAndStdinInputs) {
  base::DictValue with_persist = PtySpawnPayload("echo hi", 80, 24);
  base::DictValue persist;
  persist.Set("logPath", "C:\\logs\\out.log");
  with_persist.Set("persist", std::move(persist));
  Dispatch("spawn", std::move(with_persist));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_NE(delegate_.last_error_message.find("pty"), std::string::npos);

  base::DictValue with_stdin = PtySpawnPayload("echo hi", 80, 24);
  with_stdin.Set("stdinText", "dir\r");
  Dispatch("spawn", std::move(with_stdin));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue with_close = PtySpawnPayload("echo hi", 80, 24);
  with_close.Set("closeStdin", true);
  Dispatch("spawn", std::move(with_close));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_EQ(delegate_.error_count, 3);
}

TEST_F(NexusShellBridgeAdapterTest, PtySizeMustBeWithinRange) {
  Dispatch("spawn", PtySpawnPayload("echo hi", 0, 24));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  Dispatch("spawn",
           PtySpawnPayload("echo hi", 80,
                           NexusShellBridgeAdapter::kMaxPtyCells + 1));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue missing_rows = PtySpawnPayload("echo hi", 80, 24);
  missing_rows.FindDict("pty")->Remove("rows");
  Dispatch("spawn", std::move(missing_rows));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_EQ(delegate_.error_count, 3);

  // In range: the form is accepted and the spawn reaches the launch step.
  Dispatch("spawn", PtySpawnPayload("echo hi", 80, 24));
  EXPECT_EQ(delegate_.last_error_code, "SHELL_UNAVAILABLE");
}

TEST_F(NexusShellBridgeAdapterTest, ResizeChecksTheSizeAndReportsUnknownIds) {
  base::DictValue too_small;
  too_small.Set("streamId", "stream-1");
  too_small.Set("cols", 0);
  too_small.Set("rows", 24);
  Dispatch("resize", std::move(too_small));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue unknown;
  unknown.Set("streamId", "stream-does-not-exist");
  unknown.Set("cols", 120);
  unknown.Set("rows", 40);
  Dispatch("resize", std::move(unknown));
  ASSERT_EQ(delegate_.success_count, 1);
  EXPECT_EQ(delegate_.last_success.GetDict().FindBool("found"), false);
}

#if BUILDFLAG(IS_WIN)

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

// A pty stream dies with its adapter like a pipe stream: the dropped
// controller makes the service kill the console child's whole tree.
TEST_F(NexusShellBridgeAdapterTest, TeardownKillsAPtyStream) {
  if (!PtyConsoleHost::Get().available()) {
    GTEST_SKIP() << "no pseudoconsole host on this machine";
  }
  mojo::PendingRemote<mojom::NexusShellService> service_remote;
  NexusShellServiceImpl service(
      service_remote.InitWithNewPipeAndPassReceiver());

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusShellBridgeAdapter>(&delegate, nullptr);
  adapter->SetShellServiceForTesting(std::move(service_remote));

  base::ScopedTempDir cwd;
  ASSERT_TRUE(cwd.CreateUniqueTempDir());
  const base::FilePath::StringType kWaitFor = FILE_PATH_LITERAL("waitfor.exe");
  const std::set<base::ProcessId> before = PidsOf(kWaitFor);
  base::DictValue payload = PtySpawnPayload(
      base::StringPrintf("waitfor VellocAdapter%u /t 300",
                         static_cast<unsigned>(::GetCurrentProcessId())),
      80, 24);
  payload.Set("cwd", cwd.GetPath().AsUTF8Unsafe());
  DispatchTo(*adapter, "spawn", std::move(payload));
  ASSERT_TRUE(WaitUntil(base::BindRepeating(
      [](RecordingDelegate* d) {
        return d->success_count + d->error_count > 0;
      },
      &delegate)))
      << "the spawn never answered";
  ASSERT_EQ(delegate.error_count, 0)
      << delegate.last_error_code << ": " << delegate.last_error_message;
  ASSERT_TRUE(delegate.last_success.GetDict().FindString("streamId"));

  base::ProcessId child = 0;
  ASSERT_TRUE(WaitUntil(base::BindRepeating(
      [](const std::set<base::ProcessId>* before,
         const base::FilePath::StringType* exe, base::ProcessId* child) {
        for (base::ProcessId pid : PidsOf(*exe)) {
          if (!before->count(pid)) {
            *child = pid;
            return true;
          }
        }
        return false;
      },
      &before, &kWaitFor, &child)))
      << "waitfor.exe never started under the pty";

  adapter.reset();
  EXPECT_TRUE(WaitUntil(base::BindRepeating(
      [](base::ProcessId pid) { return !ProcessIsAlive(pid); }, child)))
      << "waitfor.exe " << child << " outlived the adapter";
}

#endif  // BUILDFLAG(IS_WIN)

TEST_F(NexusShellBridgeAdapterTest, EnvReadsSetAndUnsetVariables) {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  ASSERT_TRUE(env->SetVar("NEXUS_SHELL_ENV_TEST", "value-1"));
  env->UnSetVar("NEXUS_SHELL_ENV_TEST_UNSET");

  base::DictValue payload;
  base::ListValue names;
  names.Append("NEXUS_SHELL_ENV_TEST");
  names.Append("NEXUS_SHELL_ENV_TEST_UNSET");
  payload.Set("names", std::move(names));
  Dispatch("env", std::move(payload));

  ASSERT_EQ(delegate_.success_count, 1);
  const base::DictValue* values =
      delegate_.last_success.GetDict().FindDict("values");
  ASSERT_TRUE(values);
  EXPECT_EQ(*values->FindString("NEXUS_SHELL_ENV_TEST"), "value-1");
  const base::Value* unset = values->Find("NEXUS_SHELL_ENV_TEST_UNSET");
  ASSERT_TRUE(unset);
  EXPECT_TRUE(unset->is_none());
  env->UnSetVar("NEXUS_SHELL_ENV_TEST");
}

TEST_F(NexusShellBridgeAdapterTest, EnvRefusesTooManyNames) {
  base::DictValue payload;
  base::ListValue names;
  for (size_t i = 0; i <= NexusShellBridgeAdapter::kMaxEnvNames; ++i) {
    names.Append("N");
  }
  payload.Set("names", std::move(names));
  Dispatch("env", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

}  // namespace
}  // namespace nexus
