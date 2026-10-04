// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/streaming_child_process.h"

#include <array>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/scoped_handle.h"
#elif BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace nexus {

namespace {

// Read-chunk size for the stdout/stderr pump loops. 64 KiB matches the
// typical OS pipe buffer; a stdio MCP server's JSON-RPC frames are far
// smaller, so one frame almost always arrives in a single read.
constexpr size_t kReadChunkBytes = 64 * 1024;

// Callback fired on the binding sequence with each stdout/stderr chunk.
using ChunkCallback =
    base::RepeatingCallback<void(bool is_stdout, std::vector<uint8_t> chunk)>;

// Blocking read loop. Owns `file` (a parent-side pipe read end); reads
// until EOF / error and posts each non-empty chunk back via `on_chunk`
// bound to the binding sequence. Returns when the stream closes — which
// happens naturally once the child exits or is killed (its write end
// closes → read returns 0).
void PumpReads(base::File file, bool is_stdout, ChunkCallback on_chunk) {
  std::array<uint8_t, kReadChunkBytes> buf;
  for (;;) {
    base::span<uint8_t> buf_span(buf);
    std::optional<size_t> n = file.ReadAtCurrentPos(buf_span);
    if (!n.has_value() || *n == 0) {
      return;  // error or EOF
    }
    base::span<uint8_t> chunk_span = buf_span.first(*n);
    std::vector<uint8_t> chunk(chunk_span.begin(), chunk_span.end());
    on_chunk.Run(is_stdout, std::move(chunk));
  }
}

#if BUILDFLAG(IS_POSIX)
// Creates a blocking pipe with both ends O_CLOEXEC. The caller clears
// CLOEXEC on whichever end it hands to the child via fds_to_remap (the
// dup'd target fd is CLOEXEC-free regardless); keeping the parent end
// CLOEXEC ensures it does NOT leak into the child, so EOF detection works
// when the parent later drops its end.
bool MakePipe(base::ScopedFD* read_end, base::ScopedFD* write_end) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    PLOG(ERROR) << "[NexusShellService] pipe2 failed";
    return false;
  }
  read_end->reset(fds[0]);
  write_end->reset(fds[1]);
  return true;
}
#endif

#if BUILDFLAG(IS_WIN)
// Creates a blocking pipe. `child_end_inheritable` selects which end is
// marked inheritable (the one passed to the child); the parent end is
// always made non-inheritable so it doesn't leak into the child and break
// EOF detection.
bool MakePipe(base::win::ScopedHandle* parent_end,
              base::win::ScopedHandle* child_end,
              bool child_is_read) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE read_raw = nullptr;
  HANDLE write_raw = nullptr;
  if (!::CreatePipe(&read_raw, &write_raw, &sa, 0)) {
    PLOG(ERROR) << "[NexusShellService] CreatePipe failed";
    return false;
  }
  base::win::ScopedHandle read_h(read_raw);
  base::win::ScopedHandle write_h(write_raw);

  // child reads => child gets read end, parent keeps write end (stdin).
  // child writes => child gets write end, parent keeps read end (stdout).
  base::win::ScopedHandle& child = child_is_read ? read_h : write_h;
  base::win::ScopedHandle& parent = child_is_read ? write_h : read_h;
  // Parent end must NOT be inheritable.
  ::SetHandleInformation(parent.get(), HANDLE_FLAG_INHERIT, 0);
  *child_end = std::move(child);
  *parent_end = std::move(parent);
  return true;
}
#endif

}  // namespace

StreamingChildProcess::LaunchResult::LaunchResult() = default;
StreamingChildProcess::LaunchResult::~LaunchResult() = default;
StreamingChildProcess::LaunchResult::LaunchResult(LaunchResult&&) = default;
StreamingChildProcess::LaunchResult&
StreamingChildProcess::LaunchResult::operator=(LaunchResult&&) = default;

StreamingChildProcess::StreamingChildProcess(
    ChildProcessRequest request,
    mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
    base::OnceClosure on_closed)
    : request_(std::move(request)),
      client_(std::move(client)),
      on_closed_(std::move(on_closed)) {
  controller_.Bind(std::move(controller));
  controller_.set_disconnect_handler(
      base::BindOnce(&StreamingChildProcess::OnControllerDisconnect,
                     weak_factory_.GetWeakPtr()));
  client_.set_disconnect_handler(
      base::BindOnce(&StreamingChildProcess::OnClientDisconnect,
                     weak_factory_.GetWeakPtr()));
}

StreamingChildProcess::~StreamingChildProcess() = default;

void StreamingChildProcess::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Each blocking loop gets its own sequence; the child-process waits +
  // pipe reads/writes are sync primitives, hence WithBaseSyncPrimitives.
  const base::TaskTraits kBlockingTraits = {
      base::MayBlock(), base::WithBaseSyncPrimitives(),
      base::TaskPriority::USER_VISIBLE,
      base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};
  writer_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);
  stdout_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);
  stderr_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);
  wait_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kBlockingTraits,
      base::BindOnce(&StreamingChildProcess::LaunchOnWorker, request_),
      base::BindOnce(&StreamingChildProcess::OnLaunched,
                     weak_factory_.GetWeakPtr()));
}

// static
StreamingChildProcess::LaunchResult StreamingChildProcess::LaunchOnWorker(
    ChildProcessRequest request) {
  LaunchResult result;

  base::CommandLine cmdline(request.exe);
  for (const std::string& arg : request.args) {
    cmdline.AppendArg(arg);
  }

  base::LaunchOptions options;
  options.current_directory = request.cwd;
  options.environment = request.env;
  options.clear_environment = !request.inherit_environment;

#if BUILDFLAG(IS_POSIX)
  base::ScopedFD in_read, in_write, out_read, out_write, err_read, err_write;
  if (!MakePipe(&in_read, &in_write) || !MakePipe(&out_read, &out_write) ||
      !MakePipe(&err_read, &err_write)) {
    result.error_code = "LAUNCH_FAILED";
    result.error_message = "Could not create stdio pipes for streaming child";
    return result;
  }
  options.fds_to_remap.emplace_back(in_read.get(), STDIN_FILENO);
  options.fds_to_remap.emplace_back(out_write.get(), STDOUT_FILENO);
  options.fds_to_remap.emplace_back(err_write.get(), STDERR_FILENO);
  // Own process group so Kill() can killpg() the whole descendant tree.
  options.new_process_group = true;

  base::Process process = base::LaunchProcess(cmdline, options);
  // The child holds its own dups now; drop our copies of the child ends so
  // EOF propagates correctly when the child exits.
  in_read.reset();
  out_write.reset();
  err_write.reset();

  if (!process.IsValid()) {
    result.error_code = "LAUNCH_FAILED";
    result.error_message =
        "Failed to launch streaming child " + request.exe.AsUTF8Unsafe();
    return result;
  }
  result.stdin_write = base::File(in_write.release());
  result.stdout_read = base::File(out_read.release());
  result.stderr_read = base::File(err_read.release());
  result.process = std::move(process);
  result.ok = true;
  return result;
#elif BUILDFLAG(IS_WIN)
  base::win::ScopedHandle in_parent, in_child, out_parent, out_child,
      err_parent, err_child;
  if (!MakePipe(&in_parent, &in_child, /*child_is_read=*/true) ||
      !MakePipe(&out_parent, &out_child, /*child_is_read=*/false) ||
      !MakePipe(&err_parent, &err_child, /*child_is_read=*/false)) {
    result.error_code = "LAUNCH_FAILED";
    result.error_message = "Could not create stdio pipes for streaming child";
    return result;
  }

  options.start_hidden = true;
  options.stdin_handle = in_child.get();
  options.stdout_handle = out_child.get();
  options.stderr_handle = err_child.get();
  options.handles_to_inherit.push_back(in_child.get());
  options.handles_to_inherit.push_back(out_child.get());
  options.handles_to_inherit.push_back(err_child.get());

  // NOTE (build-pending): unlike the one-shot RunChildProcess path, this
  // uses base::LaunchProcess + start_hidden rather than the custom
  // CREATE_NO_WINDOW CreateProcessW path. start_hidden can briefly flash a
  // console window for a freshly-allocated console child; acceptable for a
  // long-lived background MCP server. If flicker proves a problem, factor
  // child_process_runner.cc's LaunchChildWithoutWindow into a shared
  // helper and reuse it here.
  base::Process process = base::LaunchProcess(cmdline, options);
  // Child owns its dups; drop our copies of the child ends.
  in_child.Close();
  out_child.Close();
  err_child.Close();

  // A KILL_ON_JOB_CLOSE job, so Kill() (and teardown) take the whole tree —
  // a shell's grandchildren included — the way child_process_runner.cc does
  // for the one-shot path. Best effort: without it Kill() terminates the
  // child alone.
  if (process.IsValid()) {
    HANDLE raw_job = ::CreateJobObjectW(nullptr, nullptr);
    if (raw_job) {
      base::win::ScopedHandle job(raw_job);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (::SetInformationJobObject(job.get(),
                                    JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits)) &&
          ::AssignProcessToJobObject(job.get(), process.Handle())) {
        result.job = std::move(job);
      } else {
        PLOG(WARNING) << "[StreamingChildProcess] job setup failed; Kill() "
                         "will terminate the child only";
      }
    }
  }

  if (!process.IsValid()) {
    result.error_code = "LAUNCH_FAILED";
    result.error_message =
        "Failed to launch streaming child " + request.exe.AsUTF8Unsafe();
    return result;
  }
  result.stdin_write = base::File(in_parent.release());
  result.stdout_read = base::File(out_parent.release());
  result.stderr_read = base::File(err_parent.release());
  result.process = std::move(process);
  result.ok = true;
  return result;
#else
  result.error_code = "LAUNCH_FAILED";
  result.error_message = "Streaming child unsupported on this platform";
  return result;
#endif
}

void StreamingChildProcess::OnLaunched(LaunchResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!result.ok) {
    // Surface the launch failure as a synthetic killed exit, then close.
    if (client_) {
      auto exit = mojom::ShellExitInfo::New();
      exit->killed = true;
      exit->duration_ms = 0;
      client_->OnExit(std::move(exit));
    }
    RequestClose();
    return;
  }

  launched_ = true;

  // The reader pumps run on worker sequences but `OnReadChunk` touches
  // sequence-affine state (the WeakPtr + the `client_` Remote), so the
  // callback must hop back to this (the binding) sequence on every chunk.
  // BindPostTask wraps the RepeatingCallback so each Run() re-posts here.
  ChunkCallback on_chunk = base::BindPostTask(
      base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindRepeating(&StreamingChildProcess::OnReadChunk,
                          weak_factory_.GetWeakPtr()));

  // Hand each parent pipe end to its dedicated pump sequence.
  stdout_runner_->PostTask(
      FROM_HERE, base::BindOnce(&PumpReads, std::move(result.stdout_read),
                                /*is_stdout=*/true, on_chunk));
  stderr_runner_->PostTask(
      FROM_HERE, base::BindOnce(&PumpReads, std::move(result.stderr_read),
                                /*is_stdout=*/false, on_chunk));

  // The writer sequence owns the stdin write end for its lifetime; stash a
  // shared holder so WriteStdin / CloseStdin tasks can reach it. A
  // RefCountedData<base::File> keeps it alive across posted tasks without
  // copying the (non-copyable) File.
  auto stdin_holder = base::MakeRefCounted<base::RefCountedData<base::File>>(
      std::move(result.stdin_write));
  stdin_holder_ = stdin_holder;

  // Flush any stdin queued before launch finished.
  for (auto& buffered : pending_stdin_) {
    writer_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<base::RefCountedData<base::File>> holder,
               std::vector<uint8_t> bytes) {
              if (holder->data.IsValid()) {
                holder->data.WriteAtCurrentPos(base::span(bytes));
              }
            },
            stdin_holder, std::move(buffered)));
  }
  pending_stdin_.clear();
  if (stdin_closed_) {
    writer_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<base::RefCountedData<base::File>> holder) {
              holder->data.Close();
            },
            stdin_holder));
  }

  // POSIX: capture pgid (== pid because new_process_group) for killpg.
  // Windows: keep the Process for Terminate(). Move the Process to the
  // wait sequence to block on exit; Kill() signals via the captured id.
#if BUILDFLAG(IS_POSIX)
  child_pgid_ = result.process.Pid();
#elif BUILDFLAG(IS_WIN)
  job_ = std::move(result.job);
  kill_process_ = result.process.Duplicate();
  if (kill_requested_) {
    // Killed before the launch finished: honour it now.
    kill_requested_ = false;
    Kill();
  }
#endif
  const base::TimeTicks start = base::TimeTicks::Now();
  wait_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::Process process, base::TimeTicks start) {
            int code = 0;
            const bool ok = process.WaitForExit(&code);
            ChildProcessOutcome outcome;
            outcome.launched = true;
            if (ok) {
              outcome.code = code;
            } else {
              outcome.killed = true;
            }
            outcome.duration = base::TimeTicks::Now() - start;
            return outcome;
          },
          std::move(result.process), start),
      base::BindOnce(&StreamingChildProcess::OnChildExited,
                     weak_factory_.GetWeakPtr()));
}

void StreamingChildProcess::OnReadChunk(bool is_stdout,
                                        std::vector<uint8_t> chunk) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!client_ || chunk.empty()) {
    return;
  }
  if (is_stdout) {
    client_->OnStdout(std::move(chunk));
  } else {
    client_->OnStderr(std::move(chunk));
  }
}

void StreamingChildProcess::OnChildExited(ChildProcessOutcome outcome) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (client_) {
    auto exit = mojom::ShellExitInfo::New();
    exit->code = outcome.code;
    exit->signal = outcome.signal;
    exit->killed = outcome.killed || kill_requested_;
    exit->timed_out = false;
    exit->duration_ms =
        static_cast<uint64_t>(outcome.duration.InMilliseconds());
    client_->OnExit(std::move(exit));
  }
  RequestClose();
}

void StreamingChildProcess::WriteStdin(const std::vector<uint8_t>& data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closing_ || stdin_closed_) {
    return;
  }
  if (!launched_) {
    pending_stdin_.push_back(data);
    return;
  }
  writer_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<base::File>> holder,
             std::vector<uint8_t> bytes) {
            if (holder->data.IsValid()) {
              holder->data.WriteAtCurrentPos(base::span(bytes));
            }
          },
          stdin_holder_, data));
}

void StreamingChildProcess::CloseStdin() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (stdin_closed_) {
    return;
  }
  stdin_closed_ = true;
  if (!launched_) {
    return;  // flushed in OnLaunched
  }
  writer_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<base::File>> holder) {
            holder->data.Close();
          },
          stdin_holder_));
}

void StreamingChildProcess::Kill() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (kill_requested_) {
    return;
  }
  kill_requested_ = true;
#if BUILDFLAG(IS_POSIX)
  if (child_pgid_ > 0) {
    ::killpg(child_pgid_, SIGKILL);
  }
#elif BUILDFLAG(IS_WIN)
  // Until 2026-10 this was a no-op on Windows (a "build-pending" note), so
  // shell.streamKill — and every streaming child's Kill — never ended a
  // Windows child that did not exit on its own. Terminate the job (the
  // whole tree) or, without one, the child.
  if (job_.is_valid()) {
    ::TerminateJobObject(job_.get(), 1);
  } else if (kill_process_.IsValid()) {
    kill_process_.Terminate(1, /*wait=*/false);
  }
#endif
  // The wait sequence observes the exit and fires OnExit + RequestClose.
}

void StreamingChildProcess::OnControllerDisconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Browser dropped the controller — kill the child and tear down.
  Kill();
  RequestClose();
}

void StreamingChildProcess::OnClientDisconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Kill();
  RequestClose();
}

void StreamingChildProcess::RequestClose() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closing_) {
    return;
  }
  closing_ = true;
  controller_.reset();
  client_.reset();
  if (on_closed_) {
    std::move(on_closed_).Run();
  }
}

}  // namespace nexus
