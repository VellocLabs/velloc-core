// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/nexus_shell_service_impl.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "mojo/public/cpp/base/big_buffer.h"
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
