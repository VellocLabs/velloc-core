// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/nexus_persistent_shells.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_simple_task_runner.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Stands in for the utility's streaming child: records what the registry
// asks of the controller.
class FakeController : public mojom::NexusStreamingProcess {
 public:
  void WriteStdin(const std::vector<uint8_t>& data) override {
    stdin_bytes.insert(stdin_bytes.end(), data.begin(), data.end());
  }
  void CloseStdin() override { stdin_closed = true; }
  void Kill() override { ++kill_count; }
  void Resize(mojom::PtySizePtr /*size*/) override {}

  std::vector<uint8_t> stdin_bytes;
  bool stdin_closed = false;
  int kill_count = 0;
};

class NexusPersistentShellsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    shells_ = std::make_unique<NexusPersistentShells>(
        base::BindRepeating(&NexusPersistentShellsTest::FakeSpawn,
                            base::Unretained(this)),
        base::BindRepeating(
            [](std::vector<std::pair<std::string, bool>>* log,
               const std::string& token, bool retain) {
              log->emplace_back(token, retain);
            },
            &keep_alive_log_));
  }

  void FakeSpawn(mojom::ShellSpawnOptionsPtr options,
                 mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
                 mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
                 NexusPersistentShells::SpawnReply reply) {
    last_command_ = options->command;
    last_args_ = options->args;
    if (reject_next_) {
      std::move(reply).Run(false, std::string("COMMAND_NOT_ALLOWED"),
                           std::string("not on the allowlist"), std::nullopt);
      return;
    }
    controller_receiver_.Bind(std::move(controller));
    client_.Bind(std::move(client));
    std::move(reply).Run(true, std::nullopt, std::nullopt, std::nullopt);
  }

  NexusPersistentShells::StartResult Start(const std::string& log_name) {
    auto options = mojom::ShellSpawnOptions::New();
    options->command = "node";
    options->args = {"server.js"};
    std::optional<NexusPersistentShells::StartResult> result;
    shells_->Start(std::move(options), LogPath(log_name), "node server.js",
                   std::string("input"), /*close_stdin=*/true,
                   base::BindOnce(
                       [](std::optional<NexusPersistentShells::StartResult>* out,
                          NexusPersistentShells::StartResult r) {
                         *out = std::move(r);
                       },
                       &result));
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(result.has_value());
    return result ? std::move(*result) : NexusPersistentShells::StartResult();
  }

  base::FilePath LogPath(const std::string& name) const {
    return temp_dir_.GetPath().AppendASCII("logs").AppendASCII(name);
  }

  std::string ReadLog(const std::string& name) const {
    std::string content;
    base::ReadFileToString(LogPath(name), &content);
    return content;
  }

  static std::vector<uint8_t> Bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  std::unique_ptr<NexusPersistentShells> shells_;
  std::vector<std::pair<std::string, bool>> keep_alive_log_;
  bool reject_next_ = false;
  std::string last_command_;
  std::vector<std::string> last_args_;
  FakeController controller_;
  mojo::Receiver<mojom::NexusStreamingProcess> controller_receiver_{
      &controller_};
  mojo::Remote<mojom::NexusStreamingProcessClient> client_;
};

TEST_F(NexusPersistentShellsTest, OutputGoesToTheLogAndTheExitIsRecorded) {
  const NexusPersistentShells::StartResult started = Start("server.log");
  ASSERT_TRUE(started.ok) << started.error_message;
  EXPECT_EQ(started.id.rfind("persist-", 0), 0u);
  EXPECT_EQ(last_command_, "node");
  EXPECT_EQ(last_args_, std::vector<std::string>{"server.js"});
  EXPECT_EQ(std::string(controller_.stdin_bytes.begin(),
                        controller_.stdin_bytes.end()),
            "input");
  EXPECT_TRUE(controller_.stdin_closed);
  // The utility is held while the child runs.
  ASSERT_EQ(keep_alive_log_.size(), 1u);
  EXPECT_EQ(keep_alive_log_[0],
            std::make_pair("persist:" + started.id, true));

  client_->OnStdout(Bytes("hello "));
  client_->OnStderr(Bytes("world"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(ReadLog("server.log"), "hello world");

  std::vector<NexusPersistentShells::Record> listed = shells_->List();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_TRUE(listed[0].running);
  EXPECT_EQ(listed[0].command, "node server.js");
  EXPECT_EQ(listed[0].log_path, LogPath("server.log").AsUTF8Unsafe());

  auto exit = mojom::ShellExitInfo::New();
  exit->code = 3;
  exit->duration_ms = 42;
  client_->OnExit(std::move(exit));
  task_environment_.RunUntilIdle();

  listed = shells_->List();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_FALSE(listed[0].running);
  EXPECT_EQ(listed[0].exit_code, 3);
  EXPECT_FALSE(listed[0].killed);
  EXPECT_EQ(listed[0].duration_ms, 42);
  EXPECT_TRUE(listed[0].ended_ms.has_value());
  ASSERT_EQ(keep_alive_log_.size(), 2u);
  EXPECT_EQ(keep_alive_log_[1],
            std::make_pair("persist:" + started.id, false));
}

// A chatty child: chunks that arrive while an append is in flight go as one
// write, in order — not one queued task (and one open/close) per chunk.
TEST_F(NexusPersistentShellsTest, OutputWhileAnAppendIsInFlightIsCoalesced) {
  ASSERT_TRUE(Start("chatty.log").ok);
  auto log_runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  shells_->SetLogTaskRunnerForTesting(log_runner);

  std::string expected;
  for (int i = 0; i < 1000; ++i) {
    const std::string chunk = "line " + base::NumberToString(i) + "\n";
    expected += chunk;
    client_->OnStdout(Bytes(chunk));
  }
  task_environment_.RunUntilIdle();
  // The first chunk's append; the other 999 wait behind it.
  EXPECT_EQ(log_runner->NumPendingTasks(), 1u);

  int appends = 0;
  while (log_runner->HasPendingTask()) {
    log_runner->RunPendingTasks();
    ++appends;
    task_environment_.RunUntilIdle();  // the reply sends the next batch
  }
  EXPECT_EQ(appends, 2);
  EXPECT_EQ(ReadLog("chatty.log"), expected);
}

// Past the cap the output is dropped, not buffered without bound, and the
// log marks the gap where it is.
TEST_F(NexusPersistentShellsTest, OutputPastTheCapIsDroppedAndMarked) {
  ASSERT_TRUE(Start("flood.log").ok);
  auto log_runner = base::MakeRefCounted<base::TestSimpleTaskRunner>();
  shells_->SetLogTaskRunnerForTesting(log_runner);

  client_->OnStdout(Bytes("first\n"));  // in flight
  const size_t mib = 1024 * 1024;
  const size_t fits = NexusPersistentShells::kMaxPendingLogBytes / mib;
  for (size_t i = 0; i < fits; ++i) {
    client_->OnStdout(std::vector<uint8_t>(mib, 'a'));
  }
  client_->OnStdout(std::vector<uint8_t>(mib, 'b'));  // over the cap
  client_->OnStdout(Bytes("x"));  // dropped too: the gap stays one gap
  task_environment_.RunUntilIdle();
  while (log_runner->HasPendingTask()) {
    log_runner->RunPendingTasks();
    task_environment_.RunUntilIdle();
  }
  client_->OnStdout(Bytes("after\n"));
  task_environment_.RunUntilIdle();
  while (log_runner->HasPendingTask()) {
    log_runner->RunPendingTasks();
    task_environment_.RunUntilIdle();
  }

  const std::string log = ReadLog("flood.log");
  const std::string marker =
      "\n[velloc: " + base::NumberToString(mib + 1) +
      " bytes of output dropped]\n";
  EXPECT_EQ(log, "first\n" + std::string(fits * mib, 'a') + marker +
                     "after\n");
}

TEST_F(NexusPersistentShellsTest, KillAsksTheControllerAndRecordsTheKill) {
  const NexusPersistentShells::StartResult started = Start("k.log");
  ASSERT_TRUE(started.ok);

  EXPECT_EQ(shells_->Kill(started.id), std::make_pair(true, true));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(controller_.kill_count, 1);

  auto exit = mojom::ShellExitInfo::New();
  exit->killed = true;
  client_->OnExit(std::move(exit));
  task_environment_.RunUntilIdle();
  const std::vector<NexusPersistentShells::Record> listed = shells_->List();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_TRUE(listed[0].killed);
  EXPECT_FALSE(listed[0].running);

  // Idempotent once it is gone; an unknown id is not found.
  EXPECT_EQ(shells_->Kill(started.id), std::make_pair(true, false));
  EXPECT_EQ(shells_->Kill("persist-unknown"), std::make_pair(false, false));
}

TEST_F(NexusPersistentShellsTest, APreLaunchRejectionLeavesNoRecord) {
  reject_next_ = true;
  const NexusPersistentShells::StartResult started = Start("never.log");
  EXPECT_FALSE(started.ok);
  EXPECT_EQ(started.error_code, "COMMAND_NOT_ALLOWED");
  EXPECT_TRUE(shells_->List().empty());
  // Retained for the launch, released by the rejection.
  ASSERT_EQ(keep_alive_log_.size(), 2u);
  EXPECT_FALSE(keep_alive_log_[1].second);
}

TEST_F(NexusPersistentShellsTest, ALostServiceRecordsTheChildAsKilled) {
  const NexusPersistentShells::StartResult started = Start("lost.log");
  ASSERT_TRUE(started.ok);
  client_.reset();  // the utility process went away
  task_environment_.RunUntilIdle();
  const std::vector<NexusPersistentShells::Record> listed = shells_->List();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_FALSE(listed[0].running);
  EXPECT_TRUE(listed[0].killed);
}

}  // namespace
}  // namespace nexus
