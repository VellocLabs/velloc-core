// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_PERSISTENT_SHELLS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_PERSISTENT_SHELLS_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

class Profile;

namespace base {
class SequencedTaskRunner;
}

namespace nexus {

// The children `shell.spawn({persist: {logPath}})` starts: streaming children
// of the shell utility (NexusShellService::StreamSpawn) whose life is NOT the
// bridge connection's. They keep running when the guest goes away; their
// stdout and stderr are appended to `logPath`; `shell.persistentList` finds
// them again and `shell.persistentKill` stops them. The exit is recorded
// here, so a guest that comes back later learns how a child ended.
//
// Per Profile (GetPersistentShells), like the shared launcher whose utility
// process runs the children: they live as long as the browser process — the
// utility's job object takes them down with it — not past it. While any is
// running, the launcher is kept from idle-recycling the utility (which would
// kill them) through the same retain tokens background shells use.
//
// This is the primitive the worker's own background-task registry is meant
// to build on (native-primitives.md: the C++ background-shell registry behind
// shell.status / shell.kill is business logic slated to move).
//
// UI thread only. Log writes hop to one sequenced MayBlock runner, so the
// bytes of a child land in the order they arrived.
class NexusPersistentShells {
 public:
  using SpawnReply = base::OnceCallback<void(
      bool ok,
      const std::optional<std::string>& error_code,
      const std::optional<std::string>& error_message,
      const std::optional<std::string>& install_hint)>;
  // NexusShellService::StreamSpawn, injectable for tests.
  using StreamSpawnFunction = base::RepeatingCallback<void(
      mojom::ShellSpawnOptionsPtr options,
      mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
      mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
      SpawnReply reply)>;
  // Retain (true) / release (false) the utility process for `token`.
  using KeepAliveFunction =
      base::RepeatingCallback<void(const std::string& token, bool retain)>;

  struct Record {
    Record();
    Record(const Record&);
    Record& operator=(const Record&);
    ~Record();

    std::string id;
    // What runs, for display: `bash -c <command>` or `<file> <args…>`.
    std::string command;
    std::string log_path;  // UTF-8
    double started_ms = 0;
    bool running = true;
    // Set once the child ended.
    std::optional<int32_t> exit_code;
    std::optional<int32_t> signal;
    bool killed = false;
    bool timed_out = false;
    std::optional<double> ended_ms;
    std::optional<double> duration_ms;
  };

  struct StartResult {
    StartResult();
    StartResult(StartResult&&);
    StartResult& operator=(StartResult&&);
    ~StartResult();

    bool ok = false;
    std::string id;
    std::string error_code;
    std::string error_message;
  };
  using StartCallback = base::OnceCallback<void(StartResult)>;

  // Record cap (running + exited). The oldest EXITED record is evicted past
  // it; a start that would need to evict a running one is refused.
  static constexpr size_t kMaxRecords = 64;

  NexusPersistentShells(StreamSpawnFunction spawn,
                        KeepAliveFunction keep_alive);
  ~NexusPersistentShells();

  NexusPersistentShells(const NexusPersistentShells&) = delete;
  NexusPersistentShells& operator=(const NexusPersistentShells&) = delete;

  // Launches `options` (already validated and allowlist-shaped by the
  // adapter) with output appended to `log_path` (absolute). `stdin_text` is
  // written right after launch; `close_stdin` then closes it.
  void Start(mojom::ShellSpawnOptionsPtr options,
             const base::FilePath& log_path,
             std::string display_command,
             std::optional<std::string> stdin_text,
             bool close_stdin,
             StartCallback callback);

  // Newest first.
  std::vector<Record> List() const;

  // {found, was_running}. The exit (killed=true) is recorded when the
  // service reports it.
  std::pair<bool, bool> Kill(const std::string& id);

  // Replaces the log task runner (tests flush it deterministically).
  void SetLogTaskRunnerForTesting(
      scoped_refptr<base::SequencedTaskRunner> runner);

 private:
  class Child;
  friend class Child;

  void OnSpawned(std::string id,
                 StartCallback callback,
                 bool ok,
                 const std::optional<std::string>& error_code,
                 const std::optional<std::string>& error_message,
                 const std::optional<std::string>& install_hint);
  void AppendToLog(const std::string& id, std::vector<uint8_t> data);
  void OnChildExit(const std::string& id, const mojom::ShellExitInfo& exit);
  bool EvictOneExited();

  StreamSpawnFunction spawn_;
  KeepAliveFunction keep_alive_;
  scoped_refptr<base::SequencedTaskRunner> log_runner_;

  std::map<std::string, Record> records_;
  std::deque<std::string> order_;  // ids, oldest first
  std::map<std::string, std::unique_ptr<Child>> children_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusPersistentShells> weak_factory_{this};
};

// The per-Profile registry, bound to the shared NexusShellServiceLauncher
// (lazily created, Profile user data). UI thread only.
NexusPersistentShells& GetPersistentShells(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_PERSISTENT_SHELLS_H_
