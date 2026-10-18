// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_PTY_CHILD_PROCESS_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_PTY_CHILD_PROCESS_H_

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/win/object_watcher.h"
#include "base/win/scoped_handle.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"
#include "custom_browser/utility/nexus/shell/pty_console_host.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

// A long-lived child running under a Windows pseudoconsole (F-31): the
// terminal's shell. Lives on the binding sequence; the blocking reader,
// writer and console-close hops run on thread-pool sequences and post back.
// Owned by `NexusShellServiceImpl`; erased via `on_closed`.

// Contract: `OnStdout` carries the console's merged output (never
// `OnStderr`), coalesced for up to ~8 ms or 32 KiB per push; `WriteStdin`
// feeds the console's input (VT: `\r` for Enter, `\x03` for Ctrl+C);
// `CloseStdin` is a no-op; `Resize` changes the console size; `Kill`
// terminates the whole process tree. `OnExit` fires exactly once, last,
// after the child has exited (or was killed) and the console is closed.
class PtyChildProcess : public mojom::NexusStreamingProcess,
                        public base::win::ObjectWatcher::Delegate {
 public:
  // The flag combination `CreateProcessW` gets. Exposed so a test can prove
  // the production choice keeps this process's redirected stdio out of the
  // child; production always uses the defaults.
  struct LaunchFlags {
    LaunchFlags();
    LaunchFlags(const LaunchFlags&);
    LaunchFlags& operator=(const LaunchFlags&);
    ~LaunchFlags();

    // STARTF_USESTDHANDLES with null std handles: the child takes its std
    // handles from the pseudoconsole instead of this process's own, which
    // may be redirected.
    bool unset_std_handles = true;
    // Runs on the launch worker once the child is in the terminal's job and
    // before its first instruction runs, with the child's process handle and
    // the job. Test-only.
    base::RepeatingCallback<void(HANDLE process, HANDLE job)> on_created;
  };

  // `request` is the policy-validated launch spec (`stdin_data` / `timeout`
  // are ignored); `cols` / `rows` are already validated (1..1000); `table`
  // is the resolved console host. `on_closed` fires exactly once, on the
  // binding sequence, when the owner should erase this object (posting the
  // erase, never deleting inside the callback's frame).
  PtyChildProcess(
      ChildProcessRequest request,
      uint16_t cols,
      uint16_t rows,
      PtyConsoleHostTable table,
      mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
      mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
      base::OnceClosure on_closed);

  PtyChildProcess(const PtyChildProcess&) = delete;
  PtyChildProcess& operator=(const PtyChildProcess&) = delete;

  // Closes the console (on a worker) and kills the tree if the child is
  // still alive.
  ~PtyChildProcess() override;

  // Kicks off the (blocking) launch on a worker sequence. Call once, right
  // after construction, on the binding sequence.
  void Start();

  // mojom::NexusStreamingProcess:
  void WriteStdin(const std::vector<uint8_t>& data) override;
  void CloseStdin() override;
  void Kill() override;
  void Resize(mojom::PtySizePtr size) override;

  // base::win::ObjectWatcher::Delegate: the child process exited.
  void OnObjectSignaled(HANDLE object) override;

  static void SetLaunchFlagsForTesting(std::optional<LaunchFlags> flags);

  // How long after the child's exit the console is closed when its output
  // pipe has not broken by itself.
  static constexpr base::TimeDelta kExitDrainTimeout = base::Milliseconds(200);

 private:
  struct LaunchResult {
    LaunchResult();
    ~LaunchResult();
    LaunchResult(LaunchResult&&);
    LaunchResult& operator=(LaunchResult&&);

    base::Process process;
    // KILL_ON_JOB_CLOSE job the child (and every descendant) runs in.
    base::win::ScopedHandle job;
    HPCON console = nullptr;
    base::File input_write;
    base::File output_read;
    bool ok = false;
    std::string error_message;
  };

  // The reader's thread handle, so the console-close path can cancel a read
  // still blocked after the console is gone.
  class Reader : public base::RefCountedThreadSafe<Reader> {
   public:
    Reader();
    void SetThread(HANDLE thread);
    void CancelRead();

   private:
    friend class base::RefCountedThreadSafe<Reader>;
    ~Reader();
    base::Lock lock_;
    base::win::ScopedHandle thread_;
  };

  using ChunkCallback = base::RepeatingCallback<void(std::vector<uint8_t>)>;

  // Worker-sequence launch (blocking). Static so it owns no `this`.
  static LaunchResult LaunchOnWorker(ChildProcessRequest request,
                                     COORD size,
                                     PtyConsoleHostTable table,
                                     LaunchFlags flags);

  // Blocking read loop on the reader sequence; `on_chunk` hops to the
  // binding sequence, `on_end` fires once when the pipe breaks.
  static void PumpOutput(base::File output_read,
                         scoped_refptr<Reader> reader,
                         ChunkCallback on_chunk,
                         base::OnceClosure on_end);

  // Closes the console on a worker while the reader sequence keeps draining.
  static void CloseConsoleOnWorker(PtyConsoleHostTable table, HPCON console);

  void OnLaunched(LaunchResult result);
  void OnReadChunk(std::vector<uint8_t> chunk);
  void OnReadEnd();
  void FlushOutput();
  void BeginClose();
  void OnConsoleClosed();
  void FinishExit();
  void TerminateTree();
  void OnControllerDisconnect();
  void OnClientDisconnect();
  void RequestClose();

  ChildProcessRequest request_;
  const COORD size_;
  const PtyConsoleHostTable table_;
  mojo::Receiver<mojom::NexusStreamingProcess> controller_{this};
  mojo::Remote<mojom::NexusStreamingProcessClient> client_;
  base::OnceClosure on_closed_;

  scoped_refptr<base::SequencedTaskRunner> writer_runner_;
  scoped_refptr<base::SequencedTaskRunner> reader_runner_;
  scoped_refptr<base::SequencedTaskRunner> close_runner_;

  bool launched_ = false;
  bool closing_ = false;
  bool kill_requested_ = false;
  bool exited_ = false;
  bool read_ended_ = false;
  bool close_begun_ = false;
  bool console_closed_ = false;
  std::optional<int> exit_code_;
  base::TimeTicks start_time_;
  std::vector<std::vector<uint8_t>> pending_input_;

  // The parent's write end of the input pipe, shared with the writer
  // sequence's posted tasks.
  scoped_refptr<base::RefCountedData<base::File>> input_holder_;
  scoped_refptr<Reader> reader_;

  // Output coalescing on the binding sequence.
  std::vector<uint8_t> pending_output_;
  base::OneShotTimer flush_timer_;

  HPCON console_ = nullptr;
  base::win::ScopedHandle job_;
  base::Process process_;
  base::win::ObjectWatcher exit_watcher_;
  base::OneShotTimer drain_timer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<PtyChildProcess> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_PTY_CHILD_PROCESS_H_
