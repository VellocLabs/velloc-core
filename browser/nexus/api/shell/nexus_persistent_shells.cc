// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/nexus_persistent_shells.h"

#include <algorithm>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

namespace {

double NowMs() {
  return base::Time::Now().InMillisecondsFSinceUnixEpoch();
}

std::string KeepAliveToken(const std::string& id) {
  return "persist:" + id;
}

// Runs on the log runner: one open-append-close per batch (the chunks that
// arrived while the previous batch was being written). Keeping no handle
// open means a log the guest rotates or deletes is simply recreated.
void AppendBytes(const base::FilePath& path, std::vector<uint8_t> data) {
  base::File file(path, base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
  if (file.IsValid()) {
    file.WriteAtCurrentPosAndCheck(data);
  }
}

void EnsureParentDirectory(const base::FilePath& path) {
  base::CreateDirectory(path.DirName());
}

const char kPersistentShellsUserDataKey[] = "nexus_persistent_shells";

struct PersistentShellsUserData : public base::SupportsUserData::Data {
  explicit PersistentShellsUserData(Profile* profile)
      : shells(base::BindRepeating(
                   [](Profile* profile, mojom::ShellSpawnOptionsPtr options,
                      mojo::PendingReceiver<mojom::NexusStreamingProcess>
                          controller,
                      mojo::PendingRemote<mojom::NexusStreamingProcessClient>
                          client,
                      NexusPersistentShells::SpawnReply reply) {
                     GetSharedShellLauncher(profile).GetService()->StreamSpawn(
                         std::move(options), std::move(controller),
                         std::move(client), std::move(reply));
                   },
                   base::Unretained(profile)),
               base::BindRepeating(
                   [](Profile* profile, const std::string& token,
                      bool retain) {
                     NexusShellServiceLauncher& launcher =
                         GetSharedShellLauncher(profile);
                     if (retain) {
                       launcher.RetainBackgroundShell(token);
                     } else {
                       launcher.ReleaseBackgroundShell(token);
                     }
                   },
                   base::Unretained(profile))) {}

  NexusPersistentShells shells;
};

}  // namespace

// One persistent child: the controller remote (kill, stdin) and the client
// receiver the service pushes output and the exit into.
class NexusPersistentShells::Child : public mojom::NexusStreamingProcessClient {
 public:
  Child(NexusPersistentShells* owner, std::string id, base::FilePath log_path)
      : owner_(owner), id_(std::move(id)), log_path_(std::move(log_path)) {}
  ~Child() override = default;

  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  mojo::PendingReceiver<mojom::NexusStreamingProcess> BindController() {
    return controller_.BindNewPipeAndPassReceiver();
  }
  mojo::PendingRemote<mojom::NexusStreamingProcessClient> BindClient() {
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> remote =
        client_receiver_.BindNewPipeAndPassRemote();
    // The service died (or dropped the child without an exit push): the
    // child is gone with it, and the record must say so.
    client_receiver_.set_disconnect_handler(
        base::BindOnce(&Child::OnDisconnected, base::Unretained(this)));
    return remote;
  }

  mojom::NexusStreamingProcess* controller() { return controller_.get(); }
  const base::FilePath& log_path() const { return log_path_; }
  bool exited() const { return exited_; }

  // mojom::NexusStreamingProcessClient:
  void OnStdout(const std::vector<uint8_t>& data) override {
    owner_->AppendToLog(id_, data);
  }
  void OnStderr(const std::vector<uint8_t>& data) override {
    owner_->AppendToLog(id_, data);
  }
  void OnExit(mojom::ShellExitInfoPtr exit) override {
    if (exited_) {
      return;
    }
    exited_ = true;
    if (exit) {
      owner_->OnChildExit(id_, *exit);
      return;
    }
    mojom::ShellExitInfo unknown;
    owner_->OnChildExit(id_, unknown);
  }

 private:
  void OnDisconnected() {
    if (exited_) {
      return;
    }
    exited_ = true;
    mojom::ShellExitInfo lost;
    lost.killed = true;
    owner_->OnChildExit(id_, lost);
  }

  const raw_ptr<NexusPersistentShells> owner_;
  const std::string id_;
  const base::FilePath log_path_;
  bool exited_ = false;
  mojo::Remote<mojom::NexusStreamingProcess> controller_;
  mojo::Receiver<mojom::NexusStreamingProcessClient> client_receiver_{this};
};

NexusPersistentShells::Record::Record() = default;
NexusPersistentShells::Record::Record(const Record&) = default;
NexusPersistentShells::Record& NexusPersistentShells::Record::operator=(
    const Record&) = default;
NexusPersistentShells::Record::~Record() = default;

NexusPersistentShells::PendingLog::PendingLog() = default;
NexusPersistentShells::PendingLog::~PendingLog() = default;

NexusPersistentShells::StartResult::StartResult() = default;
NexusPersistentShells::StartResult::StartResult(StartResult&&) = default;
NexusPersistentShells::StartResult&
NexusPersistentShells::StartResult::operator=(StartResult&&) = default;
NexusPersistentShells::StartResult::~StartResult() = default;

NexusPersistentShells::NexusPersistentShells(StreamSpawnFunction spawn,
                                             KeepAliveFunction keep_alive)
    : spawn_(std::move(spawn)),
      keep_alive_(std::move(keep_alive)),
      log_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

NexusPersistentShells::~NexusPersistentShells() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Dropping the controllers kills the children (the service ties a
  // streaming child to its controller pipe). No keep-alive release here:
  // this runs as the Profile's user data is torn down, and the launcher
  // holding those tokens goes with it.
}

void NexusPersistentShells::SetLogTaskRunnerForTesting(
    scoped_refptr<base::SequencedTaskRunner> runner) {
  log_runner_ = std::move(runner);
}

void NexusPersistentShells::Start(mojom::ShellSpawnOptionsPtr options,
                                  const base::FilePath& log_path,
                                  std::string display_command,
                                  std::optional<std::string> stdin_text,
                                  bool close_stdin,
                                  StartCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (records_.size() >= kMaxRecords && !EvictOneExited()) {
    StartResult result;
    result.error_code = "TOO_MANY_PERSISTENT";
    result.error_message = "Too many persistent children are running.";
    std::move(callback).Run(std::move(result));
    return;
  }

  const std::string id =
      "persist-" + base::Uuid::GenerateRandomV4().AsLowercaseString();
  // Posted before any output can be: the log runner is sequenced, so the
  // directory exists before the first append runs.
  log_runner_->PostTask(FROM_HERE,
                        base::BindOnce(&EnsureParentDirectory, log_path));

  auto child = std::make_unique<Child>(this, id, log_path);
  mojo::PendingReceiver<mojom::NexusStreamingProcess> controller =
      child->BindController();
  mojo::PendingRemote<mojom::NexusStreamingProcessClient> client =
      child->BindClient();
  // Queued by the service until the launch completes.
  if (stdin_text.has_value() && !stdin_text->empty()) {
    child->controller()->WriteStdin(
        std::vector<uint8_t>(stdin_text->begin(), stdin_text->end()));
  }
  if (close_stdin) {
    child->controller()->CloseStdin();
  }

  Record record;
  record.id = id;
  record.command = std::move(display_command);
  record.log_path = log_path.AsUTF8Unsafe();
  record.started_ms = NowMs();
  records_[id] = std::move(record);
  order_.push_back(id);
  children_[id] = std::move(child);
  // Retained from the start: an idle recycle between the spawn and its reply
  // would kill the child before anyone knew it existed.
  keep_alive_.Run(KeepAliveToken(id), true);

  spawn_.Run(std::move(options), std::move(controller), std::move(client),
             base::BindOnce(&NexusPersistentShells::OnSpawned,
                            weak_factory_.GetWeakPtr(), id,
                            std::move(callback)));
}

void NexusPersistentShells::OnSpawned(
    std::string id,
    StartCallback callback,
    bool ok,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  StartResult result;
  if (!ok) {
    // Pre-launch rejection: the child never existed — leave no record.
    keep_alive_.Run(KeepAliveToken(id), false);
    children_.erase(id);
    records_.erase(id);
    std::erase(order_, id);
    result.error_code = error_code.value_or("SPAWN_FAILED");
    result.error_message =
        error_message.value_or("the process could not start");
    if (install_hint.has_value() && !install_hint->empty()) {
      result.error_message += " (" + *install_hint + ")";
    }
    std::move(callback).Run(std::move(result));
    return;
  }
  result.ok = true;
  result.id = std::move(id);
  std::move(callback).Run(std::move(result));
}

void NexusPersistentShells::AppendToLog(const std::string& id,
                                        std::vector<uint8_t> data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (data.empty()) {
    return;
  }
  auto it = children_.find(id);
  if (it == children_.end()) {
    return;
  }
  PendingLog& pending = pending_logs_[id];
  if (pending.path.empty()) {
    pending.path = it->second->log_path();
  }
  // Once over the cap, drop everything until the append in flight lands, so
  // the marker sits exactly where the gap is.
  if (pending.dropped > 0 ||
      pending.bytes.size() + data.size() > kMaxPendingLogBytes) {
    pending.dropped += data.size();
  } else {
    pending.bytes.insert(pending.bytes.end(), data.begin(), data.end());
  }
  if (!pending.in_flight) {
    FlushLog(id);
  }
}

void NexusPersistentShells::FlushLog(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = pending_logs_.find(id);
  if (it == pending_logs_.end()) {
    return;
  }
  PendingLog& pending = it->second;
  if (pending.bytes.empty() && pending.dropped == 0) {
    pending_logs_.erase(it);
    return;
  }
  std::vector<uint8_t> batch;
  batch.swap(pending.bytes);
  if (pending.dropped > 0) {
    const std::string marker = "\n[velloc: " +
                               base::NumberToString(pending.dropped) +
                               " bytes of output dropped]\n";
    batch.insert(batch.end(), marker.begin(), marker.end());
    pending.dropped = 0;
  }
  pending.in_flight = true;
  log_runner_->PostTaskAndReply(
      FROM_HERE, base::BindOnce(&AppendBytes, pending.path, std::move(batch)),
      base::BindOnce(&NexusPersistentShells::OnLogFlushed,
                     weak_factory_.GetWeakPtr(), id));
}

void NexusPersistentShells::OnLogFlushed(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = pending_logs_.find(id);
  if (it == pending_logs_.end()) {
    return;
  }
  it->second.in_flight = false;
  FlushLog(id);
}

void NexusPersistentShells::OnChildExit(const std::string& id,
                                        const mojom::ShellExitInfo& exit) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto record_it = records_.find(id);
  if (record_it != records_.end() && record_it->second.running) {
    Record& record = record_it->second;
    record.running = false;
    record.exit_code = exit.code;
    record.signal = exit.signal;
    record.killed = exit.killed;
    record.timed_out = exit.timed_out;
    record.ended_ms = NowMs();
    record.duration_ms = static_cast<double>(exit.duration_ms);
    keep_alive_.Run(KeepAliveToken(id), false);
  }
  // Called from inside the child's own method: delete it after this frame.
  auto child_it = children_.find(id);
  if (child_it != children_.end()) {
    std::unique_ptr<Child> gone = std::move(child_it->second);
    children_.erase(child_it);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(gone));
  }
}

bool NexusPersistentShells::EvictOneExited() {
  for (auto it = order_.begin(); it != order_.end(); ++it) {
    auto record_it = records_.find(*it);
    if (record_it == records_.end() || !record_it->second.running) {
      records_.erase(*it);
      order_.erase(it);
      return true;
    }
  }
  return false;
}

std::vector<NexusPersistentShells::Record> NexusPersistentShells::List()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<Record> out;
  out.reserve(order_.size());
  for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
    auto record_it = records_.find(*it);
    if (record_it != records_.end()) {
      out.push_back(record_it->second);
    }
  }
  return out;
}

std::pair<bool, bool> NexusPersistentShells::Kill(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto record_it = records_.find(id);
  if (record_it == records_.end()) {
    return {false, false};
  }
  auto child_it = children_.find(id);
  if (!record_it->second.running || child_it == children_.end() ||
      child_it->second->exited()) {
    return {true, false};
  }
  // The exit (killed=true) arrives through the client and is recorded there.
  child_it->second->controller()->Kill();
  return {true, true};
}

NexusPersistentShells& GetPersistentShells(Profile* profile) {
  CHECK(profile);
  auto* data = static_cast<PersistentShellsUserData*>(
      profile->GetUserData(kPersistentShellsUserDataKey));
  if (!data) {
    auto owned = std::make_unique<PersistentShellsUserData>(profile);
    data = owned.get();
    profile->SetUserData(kPersistentShellsUserDataKey, std::move(owned));
  }
  return data->shells;
}

}  // namespace nexus
