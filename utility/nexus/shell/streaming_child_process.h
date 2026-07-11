// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_STREAMING_CHILD_PROCESS_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_STREAMING_CHILD_PROCESS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

// Slice-4 long-lived streaming child process (utility-process side).
//
// Unlike `RunChildProcess` (one-shot, temp-file capture, blocking wait),
// this owns a child kept ALIVE with three live pipes:
//   * parent writes the child's stdin incrementally (WriteStdin),
//   * parent reads the child's stdout/stderr as they stream,
//   * parent observes exit.
// It is the transport a stdio MCP server runs on: one process across the
// JSON-RPC `initialize` handshake + every later `tools/call`.
//
// Implements `mojom::NexusStreamingProcess` (the browser-held controller)
// and drives a `mojo::Remote<mojom::NexusStreamingProcessClient>` (the
// browser-held observer). Lives on, and is destroyed on, the service
// binding sequence. Reader/writer/wait loops run on dedicated
// `base::MayBlock()` + `WithBaseSyncPrimitives()` sequences and post back
// to the binding sequence; the controller methods and client callbacks
// are therefore always sequence-correct without locks.
//
// Lifetime: owned by `NexusShellServiceImpl` in a keyed map. Self-requests
// erase via `on_closed` when the controller pipe drops, the client pipe
// drops, or the child exits — whichever happens first.
class StreamingChildProcess : public mojom::NexusStreamingProcess {
 public:
  // `request` is the already-policy-validated launch spec (same struct
  // the Exec path builds via PrepareExec; `stdin_data` / `timeout` are
  // ignored for streaming). `controller` is bound here; `client` receives
  // the OnStdout/OnStderr/OnExit pushes. `on_closed` fires exactly once,
  // on the binding sequence, when this object should be erased by its
  // owner (it is NOT safe to delete `this` from within the callback's
  // synchronous frame — the owner posts the erase).
  StreamingChildProcess(
      ChildProcessRequest request,
      mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
      mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
      base::OnceClosure on_closed);

  StreamingChildProcess(const StreamingChildProcess&) = delete;
  StreamingChildProcess& operator=(const StreamingChildProcess&) = delete;

  ~StreamingChildProcess() override;

  // Kicks off the (blocking) launch on a worker sequence. Must be called
  // once, immediately after construction, on the binding sequence.
  void Start();

  // mojom::NexusStreamingProcess:
  void WriteStdin(const std::vector<uint8_t>& data) override;
  void CloseStdin() override;
  void Kill() override;

 private:
  // Result of the worker-sequence launch hop: the live process + the
  // three parent-side pipe ends (stdin write, stdout read, stderr read).
  // On failure `process` is invalid and `error_*` are set.
  struct LaunchResult {
    LaunchResult();
    ~LaunchResult();
    LaunchResult(LaunchResult&&);
    LaunchResult& operator=(LaunchResult&&);

    base::Process process;
    base::File stdin_write;
    base::File stdout_read;
    base::File stderr_read;
    bool ok = false;
    std::string error_code;
    std::string error_message;
    std::string install_hint;
  };

  // Worker-sequence launch (blocking). Static so it owns no `this`.
  static LaunchResult LaunchOnWorker(ChildProcessRequest request);

  // Binding-sequence continuation once the launch finishes. Starts the
  // reader pumps + the exit watcher, flushes any queued stdin, or fires
  // OnExit(launch-failed) + closes if launch failed.
  void OnLaunched(LaunchResult result);

  // Posted from the stdout/stderr reader sequences with each chunk; runs
  // on the binding sequence and forwards to the client. `is_stdout`
  // selects the OnStdout vs OnStderr push.
  void OnReadChunk(bool is_stdout, std::vector<uint8_t> chunk);

  // Posted from the wait sequence when the child exits. Runs on the
  // binding sequence: pushes OnExit, then requests close.
  void OnChildExited(ChildProcessOutcome outcome);

  // Mojo disconnect handlers for the controller / client pipes.
  void OnControllerDisconnect();
  void OnClientDisconnect();

  // Fires `on_closed_` once (guarded by `closing_`). Tears down pipes so
  // the reader/writer loops unblock and exit.
  void RequestClose();

  ChildProcessRequest request_;
  mojo::Receiver<mojom::NexusStreamingProcess> controller_{this};
  mojo::Remote<mojom::NexusStreamingProcessClient> client_;
  base::OnceClosure on_closed_;

  // Worker sequences. stdin writes serialize on `writer_runner_`; the two
  // readers + the exit watcher each own a sequence so a slow stream never
  // starves another.
  scoped_refptr<base::SequencedTaskRunner> writer_runner_;
  scoped_refptr<base::SequencedTaskRunner> stdout_runner_;
  scoped_refptr<base::SequencedTaskRunner> stderr_runner_;
  scoped_refptr<base::SequencedTaskRunner> wait_runner_;

  // Set once the launch completes. Until then WriteStdin/CloseStdin calls
  // buffer into `pending_stdin_`.
  bool launched_ = false;
  bool stdin_closed_ = false;
  bool closing_ = false;
  bool kill_requested_ = false;
  std::vector<std::vector<uint8_t>> pending_stdin_;

  // Owns the parent-side stdin write end after launch. Shared (ref-counted)
  // so the writer sequence's posted tasks can reach the non-copyable File.
  scoped_refptr<base::RefCountedData<base::File>> stdin_holder_;

  // POSIX: the child's process-group id (== pid; `new_process_group`), used
  // by Kill() to killpg() the whole tree. Unused on Windows (tree-kill
  // there is a TODO via Job Object), hence [[maybe_unused]].
  [[maybe_unused]] base::ProcessId child_pgid_ = 0;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<StreamingChildProcess> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_STREAMING_CHILD_PROCESS_H_
