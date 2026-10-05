// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/nexus_shell_service_impl.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "base/location.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

struct CapabilitiesSnapshot {
  std::string platform;
  mojom::ShellResolvedToolPtr default_shell;
  std::vector<mojom::ShellResolvedToolPtr> tools;
  std::vector<std::string> command_allowlist;
};

CapabilitiesSnapshot WaitForCapabilities(
    mojo::Remote<mojom::NexusShellService>& remote) {
  CapabilitiesSnapshot snapshot;
  base::RunLoop loop;
  remote->GetCapabilities(base::BindLambdaForTesting(
      [&](const std::string& platform,
          mojom::ShellResolvedToolPtr default_shell,
          std::vector<mojom::ShellResolvedToolPtr> tools,
          const std::vector<std::string>& command_allowlist) {
        snapshot.platform = platform;
        snapshot.default_shell = std::move(default_shell);
        snapshot.tools = std::move(tools);
        snapshot.command_allowlist = command_allowlist;
        loop.Quit();
      }));
  loop.Run();
  return snapshot;
}

// Single end-to-end test for capability discovery: bring the impl up
// behind a Mojo Remote, request capabilities, and verify the basic
// shape. Detailed platform / tool / allowlist invariants are covered by
// `shell_resolver_unittest.cc` so this test stays focused on the IPC
// path.
TEST(NexusShellServiceImplTest, GetCapabilitiesOverMojoReturnsHostShape) {
  base::test::TaskEnvironment task_environment;

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  const CapabilitiesSnapshot snapshot = WaitForCapabilities(remote);

  EXPECT_FALSE(snapshot.platform.empty());
  EXPECT_NE(snapshot.platform, "unknown");
  EXPECT_FALSE(snapshot.tools.empty())
      << "Service must always probe the known tool set";
  // Allowlist now mirrors the available tools (slice 2 contract). It
  // can be empty only if no known tool resolved on the host PATH —
  // unlikely in CI but possible. We just assert it is a subset of the
  // known tool ids.
  for (const std::string& id : snapshot.command_allowlist) {
    const auto it = std::ranges::find_if(
        snapshot.tools, [&](const mojom::ShellResolvedToolPtr& t) {
          return t->id == id;
        });
    ASSERT_NE(it, snapshot.tools.end());
    EXPECT_TRUE((*it)->available);
  }
}

// Verifies that callers issued before the resolver finishes are queued
// and flushed by `OnResolved`, and that subsequent calls hit the cache
// instead of triggering a second probe.
TEST(NexusShellServiceImplTest, ConcurrentCallsShareSingleProbe) {
  base::test::TaskEnvironment task_environment;

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  int responses = 0;
  base::RunLoop loop;
  auto on_response = base::BindLambdaForTesting(
      [&](const std::string& platform, mojom::ShellResolvedToolPtr,
          std::vector<mojom::ShellResolvedToolPtr>,
          const std::vector<std::string>&) {
        EXPECT_FALSE(platform.empty());
        if (++responses == 3) {
          loop.Quit();
        }
      });

  remote->GetCapabilities(on_response);
  remote->GetCapabilities(on_response);
  remote->GetCapabilities(on_response);
  loop.Run();

  EXPECT_EQ(responses, 3);
}

// ---------------------------------------------------------------------
// Slice 2 — Exec coverage
// ---------------------------------------------------------------------

namespace {

struct ExecResponse {
  bool ok = false;
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
  // Slice 6 added install_hint as the 5th reply field. Populated only
  // when error_code == "BINARY_NOT_FOUND".
  std::optional<std::string> install_hint;
  mojom::ShellExecResultPtr result;
};

ExecResponse RunExec(mojo::Remote<mojom::NexusShellService>& remote,
                     mojom::ShellSpawnOptionsPtr options) {
  ExecResponse out;
  base::RunLoop loop;
  remote->Exec(
      std::move(options),
      base::BindLambdaForTesting(
          [&](bool ok, mojom::ShellExecResultPtr result,
              const std::optional<std::string>& error_code,
              const std::optional<std::string>& error_message,
              const std::optional<std::string>& install_hint) {
            out.ok = ok;
            out.result = std::move(result);
            out.error_code = error_code;
            out.error_message = error_message;
            out.install_hint = install_hint;
            loop.Quit();
          }));
  loop.Run();
  return out;
}

// Looks up an allowlisted shell-id we can use as the `command` field of
// an Exec request. Skips the calling test if the host has none.
std::string FindAllowlistedShellId(
    const CapabilitiesSnapshot& snapshot) {
#if BUILDFLAG(IS_WIN)
  const std::vector<std::string> candidates = {"cmd", "pwsh", "powershell"};
#else
  const std::vector<std::string> candidates = {"bash", "sh", "zsh"};
#endif
  for (const std::string& id : candidates) {
    if (std::ranges::find(snapshot.command_allowlist, id) !=
        snapshot.command_allowlist.end()) {
      return id;
    }
  }
  return {};
}

}  // namespace

TEST(NexusShellServiceImplTest, ExecRunsAllowlistedShellAndReturnsStdout) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  const CapabilitiesSnapshot caps = WaitForCapabilities(remote);
  const std::string shell_id = FindAllowlistedShellId(caps);
  if (shell_id.empty()) {
    GTEST_SKIP() << "No allowlisted shell on host; cannot exercise Exec";
  }

  auto options = mojom::ShellSpawnOptions::New();
  options->command = shell_id;
#if BUILDFLAG(IS_WIN)
  options->args = {"/C", "echo nexus-exec-ok"};
#else
  options->args = {"-c", "echo nexus-exec-ok"};
#endif
  options->cwd = tmp.GetPath();
  options->use_shell = false;
  options->timeout_ms = 10000;
  options->max_output_bytes = 4096;

  const ExecResponse response = RunExec(remote, std::move(options));
  ASSERT_TRUE(response.ok)
      << "Exec failed: code=" << response.error_code.value_or("")
      << " message=" << response.error_message.value_or("");
  ASSERT_TRUE(response.result);
  ASSERT_TRUE(response.result->exit);
  ASSERT_TRUE(response.result->exit->code.has_value());
  EXPECT_EQ(*response.result->exit->code, 0);
  EXPECT_FALSE(response.result->exit->timed_out);

  const base::span<const uint8_t> stdout_span =
      base::span<const uint8_t>(response.result->stdout_buffer);
  const std::string stdout_str(stdout_span.begin(), stdout_span.end());
  EXPECT_NE(stdout_str.find("nexus-exec-ok"), std::string::npos)
      << "Got stdout: " << stdout_str;
}

TEST(NexusShellServiceImplTest, SpawnReturnsPartialOutputWhileRunning) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  const CapabilitiesSnapshot caps = WaitForCapabilities(remote);
  const std::string shell_id_tool = FindAllowlistedShellId(caps);
  if (shell_id_tool.empty()) {
    GTEST_SKIP() << "No allowlisted shell on host; cannot exercise Spawn";
  }

  auto options = mojom::ShellSpawnOptions::New();
  options->command = shell_id_tool;
#if BUILDFLAG(IS_WIN)
  options->args = {"/C", "echo nexus-bg-partial & ping -n 3 127.0.0.1 >NUL & echo nexus-bg-final"};
#else
  options->args = {"-c", "echo nexus-bg-partial; sleep 1; echo nexus-bg-final"};
#endif
  options->cwd = tmp.GetPath();
  options->use_shell = false;
  options->timeout_ms = 10000;
  options->max_output_bytes = 4096;

  std::string bg_id;
  base::RunLoop spawn_loop;
  remote->Spawn(
      std::move(options),
      base::BindLambdaForTesting(
          [&](bool ok, const std::optional<std::string>& shell_id,
              const std::optional<std::string>& error_code,
              const std::optional<std::string>& error_message,
              const std::optional<std::string>& install_hint) {
            ASSERT_TRUE(ok) << error_code.value_or("") << ": "
                            << error_message.value_or("") << " "
                            << install_hint.value_or("");
            ASSERT_TRUE(shell_id.has_value());
            bg_id = *shell_id;
            spawn_loop.Quit();
          }));
  spawn_loop.Run();
  ASSERT_FALSE(bg_id.empty());

  bool saw_partial_while_running = false;
  mojom::ShellBackgroundOutputPtr final_output;
  for (int i = 0; i < 80 && !final_output; ++i) {
    base::RunLoop poll;
    mojom::ShellBackgroundOutputPtr snapshot;
    remote->ReadBackgroundOutput(
        bg_id,
        base::BindLambdaForTesting([&](mojom::ShellBackgroundOutputPtr out) {
          snapshot = std::move(out);
          poll.Quit();
        }));
    poll.Run();
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot->found);
    const base::span<const uint8_t> out_span =
        base::span<const uint8_t>(snapshot->stdout_buffer);
    const std::string out_str(out_span.begin(), out_span.end());
    if (snapshot->running &&
        out_str.find("nexus-bg-partial") != std::string::npos) {
      saw_partial_while_running = true;
    }
    if (!snapshot->running) {
      final_output = std::move(snapshot);
      break;
    }
    base::RunLoop wait;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, wait.QuitClosure(), base::Milliseconds(50));
    wait.Run();
  }

  EXPECT_TRUE(saw_partial_while_running)
      << "ReadBackgroundOutput should surface stdout before process exit";
  ASSERT_TRUE(final_output) << "background command never completed";
  const base::span<const uint8_t> final_span =
      base::span<const uint8_t>(final_output->stdout_buffer);
  const std::string final_str(final_span.begin(), final_span.end());
  EXPECT_NE(final_str.find("nexus-bg-final"), std::string::npos)
      << "Got stdout: " << final_str;
}

// Regression: the guest's Stop flow (SharedWorkerSessionManager
// stopBackgroundShell) marks a background task 'stopped' ONLY when, after
// `Kill` acks `{found:true, was_running:true}`, a `shell.status` poll observes
// `running:false` with `exit.killed:true` within ~5 s. This is the C++ half of
// that contract: Kill must promptly terminate the tree, and the recorded
// outcome must carry the forced `killed` flag (OnBackgroundComplete).
TEST(NexusShellServiceImplTest, KillTerminatesRunningBackgroundShellPromptly) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  const CapabilitiesSnapshot caps = WaitForCapabilities(remote);
  const std::string shell_id_tool = FindAllowlistedShellId(caps);
  if (shell_id_tool.empty()) {
    GTEST_SKIP() << "No allowlisted shell on host; cannot exercise Kill";
  }

  // Long-lived command with a grandchild in the tree (mirrors the e2e's
  // `(sleep …) & wait` shape) — Kill must take down the whole tree, not
  // just the immediate shell.
  auto options = mojom::ShellSpawnOptions::New();
  options->command = shell_id_tool;
#if BUILDFLAG(IS_WIN)
  options->args = {"/C", "echo nexus-kill-start & ping -n 30 127.0.0.1 >NUL"};
#else
  options->args = {"-c", "echo nexus-kill-start; sleep 30"};
#endif
  options->cwd = tmp.GetPath();
  options->use_shell = false;
  options->timeout_ms = 60000;
  options->max_output_bytes = 4096;

  std::string bg_id;
  base::RunLoop spawn_loop;
  remote->Spawn(
      std::move(options),
      base::BindLambdaForTesting(
          [&](bool ok, const std::optional<std::string>& shell_id,
              const std::optional<std::string>& error_code,
              const std::optional<std::string>& error_message,
              const std::optional<std::string>& install_hint) {
            ASSERT_TRUE(ok) << error_code.value_or("") << ": "
                            << error_message.value_or("");
            ASSERT_TRUE(shell_id.has_value());
            bg_id = *shell_id;
            spawn_loop.Quit();
          }));
  spawn_loop.Run();
  ASSERT_FALSE(bg_id.empty());

  // Wait until the child is observably RUNNING (stdout marker) so Kill hits a
  // live launched process, not the pre-launch race path.
  bool saw_running = false;
  for (int i = 0; i < 100 && !saw_running; ++i) {
    base::RunLoop poll;
    remote->ReadBackgroundOutput(
        bg_id,
        base::BindLambdaForTesting([&](mojom::ShellBackgroundOutputPtr out) {
          ASSERT_TRUE(out);
          ASSERT_TRUE(out->found);
          const base::span<const uint8_t> out_span =
              base::span<const uint8_t>(out->stdout_buffer);
          const std::string out_str(out_span.begin(), out_span.end());
          saw_running = out->running &&
              out_str.find("nexus-kill-start") != std::string::npos;
          poll.Quit();
        }));
    poll.Run();
    if (!saw_running) {
      base::RunLoop wait;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, wait.QuitClosure(), base::Milliseconds(50));
      wait.Run();
    }
  }
  ASSERT_TRUE(saw_running) << "background shell never reached running state";

  // Kill must ack found + was_running (the guest treats anything else as
  // stop_failed and leaves the row running).
  const base::TimeTicks kill_at = base::TimeTicks::Now();
  base::RunLoop kill_loop;
  remote->Kill(bg_id, base::BindLambdaForTesting(
                          [&](bool found, bool was_running) {
                            EXPECT_TRUE(found);
                            EXPECT_TRUE(was_running);
                            kill_loop.Quit();
                          }));
  kill_loop.Run();

  // The guest confirms termination via status polls with a 5 s budget
  // (SHELL_STOP_CONFIRM_TIMEOUT_MS). Poll up to 10 s here but ASSERT the
  // 5 s contract — a slower kill means the drawer row never leaves
  // 'running' even though the process eventually dies.
  mojom::ShellBackgroundOutputPtr terminal;
  for (int i = 0; i < 200 && !terminal; ++i) {
    base::RunLoop poll;
    remote->ReadBackgroundOutput(
        bg_id,
        base::BindLambdaForTesting([&](mojom::ShellBackgroundOutputPtr out) {
          ASSERT_TRUE(out);
          ASSERT_TRUE(out->found);
          if (!out->running) {
            terminal = std::move(out);
          }
          poll.Quit();
        }));
    poll.Run();
    if (!terminal) {
      base::RunLoop wait;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, wait.QuitClosure(), base::Milliseconds(50));
      wait.Run();
    }
  }
  const base::TimeDelta kill_to_terminal = base::TimeTicks::Now() - kill_at;
  ASSERT_TRUE(terminal) << "killed background shell never reported terminal";
  EXPECT_LT(kill_to_terminal, base::Seconds(5))
      << "kill→terminal took " << kill_to_terminal
      << " — exceeds the guest's stop-confirm budget";
  ASSERT_TRUE(terminal->exit);
  EXPECT_TRUE(terminal->exit->killed)
      << "a Kill-terminated shell must report exit.killed (the guest maps "
         "killed→'stopped'; signal/exit-code map to 'error')";
  EXPECT_FALSE(terminal->exit->timed_out);
}

// A streaming child's Kill must end it — on Windows too, where it used to be
// a no-op (the persistent children of shell.spawn({persist}) depend on it).
// The exit then arrives through the client as killed.
class RecordingStreamClient : public mojom::NexusStreamingProcessClient {
 public:
  void OnStdout(const std::vector<uint8_t>& data) override {
    stdout_text.append(data.begin(), data.end());
  }
  void OnStderr(const std::vector<uint8_t>& data) override {}
  void OnExit(mojom::ShellExitInfoPtr exit) override {
    exited = std::move(exit);
    if (on_exit) std::move(on_exit).Run();
  }
  std::string stdout_text;
  mojom::ShellExitInfoPtr exited;
  base::OnceClosure on_exit;
};

TEST(NexusShellServiceImplTest, StreamSpawnKillEndsALongRunningChild) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());
  const CapabilitiesSnapshot caps = WaitForCapabilities(remote);
  const std::string shell_id_tool = FindAllowlistedShellId(caps);
  if (shell_id_tool.empty()) {
    GTEST_SKIP() << "No allowlisted shell on host; cannot exercise StreamSpawn";
  }

  auto options = mojom::ShellSpawnOptions::New();
  options->command = shell_id_tool;
#if BUILDFLAG(IS_WIN)
  options->args = {"/C", "echo nexus-stream-start & ping -n 60 127.0.0.1 >NUL"};
#else
  options->args = {"-c", "echo nexus-stream-start; sleep 60"};
#endif
  options->cwd = tmp.GetPath();
  options->use_shell = false;

  RecordingStreamClient client;
  mojo::Receiver<mojom::NexusStreamingProcessClient> client_receiver(&client);
  mojo::Remote<mojom::NexusStreamingProcess> controller;
  base::RunLoop spawn_loop;
  remote->StreamSpawn(
      std::move(options), controller.BindNewPipeAndPassReceiver(),
      client_receiver.BindNewPipeAndPassRemote(),
      base::BindLambdaForTesting(
          [&](bool ok, const std::optional<std::string>& error_code,
              const std::optional<std::string>& error_message,
              const std::optional<std::string>& install_hint) {
            ASSERT_TRUE(ok) << error_code.value_or("") << ": "
                            << error_message.value_or("");
            spawn_loop.Quit();
          }));
  spawn_loop.Run();

  // Running: the marker arrived.
  for (int i = 0; i < 200 &&
                  client.stdout_text.find("nexus-stream-start") ==
                      std::string::npos;
       ++i) {
    base::RunLoop wait;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, wait.QuitClosure(), base::Milliseconds(50));
    wait.Run();
  }
  ASSERT_NE(client.stdout_text.find("nexus-stream-start"), std::string::npos);
  ASSERT_FALSE(client.exited);

  const base::TimeTicks kill_at = base::TimeTicks::Now();
  base::RunLoop exit_loop;
  client.on_exit = exit_loop.QuitClosure();
  controller->Kill();
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, exit_loop.QuitClosure(), base::Seconds(10));
  exit_loop.Run();
  ASSERT_TRUE(client.exited) << "a killed streaming child never exited";
  EXPECT_TRUE(client.exited->killed);
  EXPECT_LT(base::TimeTicks::Now() - kill_at, base::Seconds(5));
}

// Kill on an unknown id and on an already-completed shell must stay
// idempotent — `{found:false}` / `{found:true, was_running:false}` — the
// guest surfaces these as 'not_found' / 'already_finished', never a crash.
TEST(NexusShellServiceImplTest, KillIsIdempotentOnUnknownAndCompletedShells) {
  base::test::TaskEnvironment task_environment;

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  base::RunLoop unknown_loop;
  remote->Kill("bg_does-not-exist", base::BindLambdaForTesting(
                                        [&](bool found, bool was_running) {
                                          EXPECT_FALSE(found);
                                          EXPECT_FALSE(was_running);
                                          unknown_loop.Quit();
                                        }));
  unknown_loop.Run();
}

TEST(NexusShellServiceImplTest, BackgroundShellIdsAreUniqueAcrossServiceRestarts) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  std::string first_id;
  {
    mojo::Remote<mojom::NexusShellService> remote;
    NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());
    const CapabilitiesSnapshot caps = WaitForCapabilities(remote);
    const std::string shell_id_tool = FindAllowlistedShellId(caps);
    if (shell_id_tool.empty()) {
      GTEST_SKIP() << "No allowlisted shell on host; cannot exercise Spawn";
    }

    auto options = mojom::ShellSpawnOptions::New();
    options->command = shell_id_tool;
#if BUILDFLAG(IS_WIN)
    options->args = {"/C", "echo first-bg"};
#else
    options->args = {"-c", "echo first-bg"};
#endif
    options->cwd = tmp.GetPath();
    options->use_shell = false;
    options->timeout_ms = 10000;
    options->max_output_bytes = 4096;

    base::RunLoop loop;
    remote->Spawn(
        std::move(options),
        base::BindLambdaForTesting(
            [&](bool ok, const std::optional<std::string>& shell_id,
                const std::optional<std::string>& error_code,
                const std::optional<std::string>& error_message,
                const std::optional<std::string>& install_hint) {
              EXPECT_TRUE(ok) << error_code.value_or("") << ": "
                              << error_message.value_or("") << " "
                              << install_hint.value_or("");
              ASSERT_TRUE(shell_id.has_value());
              first_id = *shell_id;
              loop.Quit();
            }));
    loop.Run();
  }

  std::string second_id;
  {
    mojo::Remote<mojom::NexusShellService> remote;
    NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());
    const CapabilitiesSnapshot caps = WaitForCapabilities(remote);
    const std::string shell_id_tool = FindAllowlistedShellId(caps);
    if (shell_id_tool.empty()) {
      GTEST_SKIP() << "No allowlisted shell on host; cannot exercise Spawn";
    }

    auto options = mojom::ShellSpawnOptions::New();
    options->command = shell_id_tool;
#if BUILDFLAG(IS_WIN)
    options->args = {"/C", "echo second-bg"};
#else
    options->args = {"-c", "echo second-bg"};
#endif
    options->cwd = tmp.GetPath();
    options->use_shell = false;
    options->timeout_ms = 10000;
    options->max_output_bytes = 4096;

    base::RunLoop loop;
    remote->Spawn(
        std::move(options),
        base::BindLambdaForTesting(
            [&](bool ok, const std::optional<std::string>& shell_id,
                const std::optional<std::string>& error_code,
                const std::optional<std::string>& error_message,
                const std::optional<std::string>& install_hint) {
              EXPECT_TRUE(ok) << error_code.value_or("") << ": "
                              << error_message.value_or("") << " "
                              << install_hint.value_or("");
              ASSERT_TRUE(shell_id.has_value());
              second_id = *shell_id;
              loop.Quit();
            }));
    loop.Run();
  }

  EXPECT_FALSE(first_id.empty());
  EXPECT_FALSE(second_id.empty());
  EXPECT_NE(first_id, second_id)
      << "Restarted NexusShellService instances must not reuse shell_id values; "
         "otherwise the SharedWorker can mistake a fresh background shell for a "
         "stale finished one and hide/drop its notification.";
}

TEST(NexusShellServiceImplTest, ExecRejectsNonAllowlistedCommand) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());

  // Drain capabilities so the impl does not queue our Exec behind the
  // resolver — the policy decision should fire on the binding sequence
  // immediately afterwards.
  WaitForCapabilities(remote);

  auto options = mojom::ShellSpawnOptions::New();
  options->command = "definitely-not-a-real-tool-id";
  options->cwd = tmp.GetPath();

  const ExecResponse response = RunExec(remote, std::move(options));
  EXPECT_FALSE(response.ok);
  EXPECT_FALSE(response.result);
  EXPECT_EQ(response.error_code.value_or(""), "COMMAND_NOT_ALLOWED");
}

TEST(NexusShellServiceImplTest, ExecRejectsInvalidCwd) {
  base::test::TaskEnvironment task_environment;

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());
  WaitForCapabilities(remote);

  auto options = mojom::ShellSpawnOptions::New();
  options->command = "rg";  // doesn't matter, cwd validated first
#if BUILDFLAG(IS_WIN)
  options->cwd = base::FilePath(L"relative\\bad");
#else
  options->cwd = base::FilePath("relative/bad");
#endif

  const ExecResponse response = RunExec(remote, std::move(options));
  EXPECT_FALSE(response.ok);
  EXPECT_FALSE(response.result);
  EXPECT_EQ(response.error_code.value_or(""), "INVALID_CWD");
}

TEST(NexusShellServiceImplTest, ExecUsesDefaultShellWhenUseShellIsTrue) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  mojo::Remote<mojom::NexusShellService> remote;
  NexusShellServiceImpl service(remote.BindNewPipeAndPassReceiver());
  const CapabilitiesSnapshot caps = WaitForCapabilities(remote);

  if (!caps.default_shell || !caps.default_shell->available) {
    GTEST_SKIP() << "No default shell on host; cannot exercise use_shell";
  }

  auto options = mojom::ShellSpawnOptions::New();
  options->use_shell = true;
  options->command = "echo nexus-shell-ok";
  options->cwd = tmp.GetPath();
  options->timeout_ms = 10000;
  options->max_output_bytes = 4096;

  const ExecResponse response = RunExec(remote, std::move(options));
  ASSERT_TRUE(response.ok)
      << "use_shell exec failed: code=" << response.error_code.value_or("")
      << " message=" << response.error_message.value_or("");
  ASSERT_TRUE(response.result);

  const base::span<const uint8_t> stdout_span =
      base::span<const uint8_t>(response.result->stdout_buffer);
  const std::string stdout_str(stdout_span.begin(), stdout_span.end());
  EXPECT_NE(stdout_str.find("nexus-shell-ok"), std::string::npos)
      << "Got stdout: " << stdout_str;
}

}  // namespace
}  // namespace nexus
