// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/pty_child_process.h"

#include <array>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/environment_internal.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/win/scoped_process_information.h"
#include "base/win/startup_information.h"

namespace nexus {

namespace {

constexpr size_t kReadChunkBytes = 64 * 1024;

// Output is pushed to the client once this much is pending, or after
// `kFlushDelay` since the first pending byte, whichever comes first.
constexpr size_t kFlushBytes = 32 * 1024;
constexpr base::TimeDelta kFlushDelay = base::Milliseconds(8);

// After the console is closed, how long a still-blocked read is given to
// see the pipe break on its own before it is cancelled.
constexpr base::TimeDelta kReadEndTimeout = base::Milliseconds(500);

constexpr uint16_t kMaxCells = 1000;

constexpr base::TaskTraits kBlockingTraits = {
    base::MayBlock(), base::WithBaseSyncPrimitives(),
    base::TaskPriority::USER_VISIBLE,
    base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};

std::optional<PtyChildProcess::LaunchFlags>& LaunchFlagsOverride() {
  static base::NoDestructor<std::optional<PtyChildProcess::LaunchFlags>> flags;
  return *flags;
}

// Security: every pty child goes in a Job Object with KILL_ON_JOB_CLOSE, so
// the shell's descendants die with the terminal, on Kill or on teardown.
base::win::ScopedHandle CreateKillOnCloseJob() {
  HANDLE raw = ::CreateJobObjectW(nullptr, nullptr);
  if (!raw) {
    PLOG(WARNING) << "[PtyChildProcess] CreateJobObjectW failed";
    return base::win::ScopedHandle();
  }
  base::win::ScopedHandle job(raw);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
    PLOG(WARNING) << "[PtyChildProcess] SetInformationJobObject failed";
    return base::win::ScopedHandle();
  }
  return job;
}

}  // namespace

PtyChildProcess::LaunchFlags::LaunchFlags() = default;
PtyChildProcess::LaunchFlags::LaunchFlags(const LaunchFlags&) = default;
PtyChildProcess::LaunchFlags& PtyChildProcess::LaunchFlags::operator=(
    const LaunchFlags&) = default;
PtyChildProcess::LaunchFlags::~LaunchFlags() = default;

PtyChildProcess::LaunchResult::LaunchResult() = default;
PtyChildProcess::LaunchResult::~LaunchResult() = default;
PtyChildProcess::LaunchResult::LaunchResult(LaunchResult&&) = default;
PtyChildProcess::LaunchResult& PtyChildProcess::LaunchResult::operator=(
    LaunchResult&&) = default;

PtyChildProcess::Reader::Reader() = default;
PtyChildProcess::Reader::~Reader() = default;

void PtyChildProcess::Reader::SetThread(HANDLE thread) {
  base::AutoLock auto_lock(lock_);
  thread_.Set(thread);
}

void PtyChildProcess::Reader::CancelRead() {
  base::AutoLock auto_lock(lock_);
  if (thread_.is_valid()) {
    ::CancelSynchronousIo(thread_.get());
  }
}

PtyChildProcess::PtyChildProcess(
    ChildProcessRequest request,
    uint16_t cols,
    uint16_t rows,
    PtyConsoleHostTable table,
    mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
    base::OnceClosure on_closed)
    : request_(std::move(request)),
      size_{static_cast<SHORT>(cols), static_cast<SHORT>(rows)},
      table_(std::move(table)),
      client_(std::move(client)),
      on_closed_(std::move(on_closed)) {
  controller_.Bind(std::move(controller));
  controller_.set_disconnect_handler(base::BindOnce(
      &PtyChildProcess::OnControllerDisconnect, weak_factory_.GetWeakPtr()));
  client_.set_disconnect_handler(base::BindOnce(
      &PtyChildProcess::OnClientDisconnect, weak_factory_.GetWeakPtr()));
}

PtyChildProcess::~PtyChildProcess() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  exit_watcher_.StopWatching();
  if (process_.IsValid() && !exited_) {
    TerminateTree();
  }
  if (console_) {
    base::ThreadPool::PostTask(
        FROM_HERE, kBlockingTraits,
        base::BindOnce(&PtyChildProcess::CloseConsoleOnWorker, table_,
                       std::exchange(console_, nullptr)));
  }
}

void PtyChildProcess::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  writer_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);
  reader_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);
  close_runner_ = base::ThreadPool::CreateSequencedTaskRunner(kBlockingTraits);
  start_time_ = base::TimeTicks::Now();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kBlockingTraits,
      base::BindOnce(&PtyChildProcess::LaunchOnWorker, request_, size_, table_,
                     LaunchFlagsOverride().value_or(LaunchFlags())),
      base::BindOnce(&PtyChildProcess::OnLaunched, weak_factory_.GetWeakPtr()));
}

// static
void PtyChildProcess::SetLaunchFlagsForTesting(
    std::optional<LaunchFlags> flags) {
  LaunchFlagsOverride() = flags;
}

// static
PtyChildProcess::LaunchResult PtyChildProcess::LaunchOnWorker(
    ChildProcessRequest request,
    COORD size,
    PtyConsoleHostTable table,
    LaunchFlags flags) {
  LaunchResult result;
  if (!table.available()) {
    result.error_message = "no pseudoconsole on this host";
    return result;
  }

  // Both pipes stay non-inheritable: the console host receives its own
  // duplicates of `in_read` / `out_write` from the create call, and the
  // child never sees a pipe end at all — its stdio is the console.
  HANDLE in_read_raw = nullptr;
  HANDLE in_write_raw = nullptr;
  HANDLE out_read_raw = nullptr;
  HANDLE out_write_raw = nullptr;
  if (!::CreatePipe(&in_read_raw, &in_write_raw, nullptr, 0)) {
    PLOG(ERROR) << "[PtyChildProcess] CreatePipe(input) failed";
    result.error_message = "could not create the console input pipe";
    return result;
  }
  base::win::ScopedHandle in_read(in_read_raw);
  base::File in_write(in_write_raw);
  if (!::CreatePipe(&out_read_raw, &out_write_raw, nullptr, 0)) {
    PLOG(ERROR) << "[PtyChildProcess] CreatePipe(output) failed";
    result.error_message = "could not create the console output pipe";
    return result;
  }
  base::File out_read(out_read_raw);
  base::win::ScopedHandle out_write(out_write_raw);

  HPCON console = nullptr;
  const HRESULT hr =
      table.create(size, in_read.get(), out_write.get(), 0, &console);
  if (FAILED(hr) || !console) {
    LOG(ERROR) << "[PtyChildProcess] CreatePseudoConsole failed: 0x"
               << std::hex << hr;
    result.error_message = "could not create the pseudoconsole";
    return result;
  }
  // The console host owns its ends now.
  in_read.Close();
  out_write.Close();

  // The child joins the kill-on-close job before its first instruction, so
  // nothing it starts can escape Kill: it is created suspended, assigned,
  // then resumed. No job, no launch.
  base::win::ScopedHandle job = CreateKillOnCloseJob();
  if (!job.is_valid()) {
    table.close(console);
    result.error_message = "could not create the terminal's job object";
    return result;
  }

  base::win::StartupInformation startup_info_wrapper;
  STARTUPINFOW* startup_info = startup_info_wrapper.startup_info();
  // One attribute: the pseudoconsole. The job is not passed as an attribute:
  // a pseudoconsole client created straight into a job exits at once under
  // the shell utility process, while assigning the suspended child works.
  // No handle list and no inheritance, so the child's std handles come from
  // the console, never from this process.
  if (!startup_info_wrapper.InitializeProcThreadAttributeList(1) ||
      !startup_info_wrapper.UpdateProcThreadAttribute(
          PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(console))) {
    PLOG(ERROR) << "[PtyChildProcess] pseudoconsole attribute failed";
    table.close(console);
    result.error_message = "could not attach the pseudoconsole";
    return result;
  }
  if (flags.unset_std_handles) {
    startup_info->dwFlags |= STARTF_USESTDHANDLES;
    startup_info->hStdInput = nullptr;
    startup_info->hStdOutput = nullptr;
    startup_info->hStdError = nullptr;
  }

  // No CREATE_NO_WINDOW, CREATE_NEW_CONSOLE or DETACHED_PROCESS: each would
  // give the child a console other than the pseudoconsole.
  const DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT |
                               CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
  static const wchar_t kEmptyEnvironment[] = {0};
  base::NativeEnvironmentString env_block =
      base::internal::AlterEnvironment(kEmptyEnvironment, request.env);

  base::CommandLine cmdline(request.exe);
  for (const std::string& arg : request.args) {
    cmdline.AppendArg(arg);
  }
  std::wstring writable_cmdline = cmdline.GetCommandLineString();
  LPCWSTR current_directory =
      request.cwd.empty() ? nullptr : request.cwd.value().c_str();

  PROCESS_INFORMATION temp_process_info = {};
  if (!::CreateProcessW(nullptr, writable_cmdline.data(), nullptr, nullptr,
                        /*bInheritHandles=*/FALSE, creation_flags,
                        env_block.data(), current_directory, startup_info,
                        &temp_process_info)) {
    PLOG(ERROR) << "[PtyChildProcess] CreateProcessW failed";
    table.close(console);
    result.error_message =
        "failed to launch " + request.exe.AsUTF8Unsafe() + " under a console";
    return result;
  }
  base::win::ScopedProcessInformation process_info(temp_process_info);
  base::Process process(process_info.TakeProcessHandle());
  if (!::AssignProcessToJobObject(job.get(), process.Handle())) {
    PLOG(ERROR) << "[PtyChildProcess] AssignProcessToJobObject failed";
    process.Terminate(1, /*wait=*/false);
    table.close(console);
    result.error_message = "could not place the shell in the terminal's job";
    return result;
  }
  if (flags.on_created) {
    flags.on_created.Run(process.Handle(), job.get());
  }
  if (::ResumeThread(process_info.thread_handle()) == static_cast<DWORD>(-1)) {
    PLOG(ERROR) << "[PtyChildProcess] ResumeThread failed";
    process.Terminate(1, /*wait=*/false);
    table.close(console);
    result.error_message = "could not start the shell";
    return result;
  }

  result.process = std::move(process);
  result.job = std::move(job);
  result.console = console;
  result.input_write = std::move(in_write);
  result.output_read = std::move(out_read);
  result.ok = true;
  return result;
}

// static
void PtyChildProcess::PumpOutput(base::File output_read,
                                 scoped_refptr<Reader> reader,
                                 ChunkCallback on_chunk,
                                 base::OnceClosure on_end) {
  HANDLE thread = nullptr;
  if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                        ::GetCurrentProcess(), &thread, THREAD_TERMINATE,
                        FALSE, 0)) {
    reader->SetThread(thread);
  }
  std::array<uint8_t, kReadChunkBytes> buf;
  for (;;) {
    base::span<uint8_t> buf_span(buf);
    std::optional<size_t> n = output_read.ReadAtCurrentPos(buf_span);
    if (!n.has_value() || *n == 0) {
      break;
    }
    base::span<uint8_t> chunk_span = buf_span.first(*n);
    on_chunk.Run(std::vector<uint8_t>(chunk_span.begin(), chunk_span.end()));
  }
  std::move(on_end).Run();
}

// static
void PtyChildProcess::CloseConsoleOnWorker(PtyConsoleHostTable table,
                                           HPCON console) {
  // Blocks on hosts that wait for their last frame to be read; the reader
  // sequence is still draining, so it returns.
  if (console && table.close) {
    table.close(console);
  }
}

void PtyChildProcess::OnLaunched(LaunchResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!result.ok) {
    LOG(WARNING) << "[PtyChildProcess] launch failed: " << result.error_message;
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
  console_ = result.console;
  job_ = std::move(result.job);
  process_ = std::move(result.process);

  reader_ = base::MakeRefCounted<Reader>();
  scoped_refptr<base::SequencedTaskRunner> binding =
      base::SequencedTaskRunner::GetCurrentDefault();
  reader_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &PtyChildProcess::PumpOutput, std::move(result.output_read), reader_,
          base::BindPostTask(binding,
                             base::BindRepeating(&PtyChildProcess::OnReadChunk,
                                                 weak_factory_.GetWeakPtr())),
          base::BindPostTask(binding,
                             base::BindOnce(&PtyChildProcess::OnReadEnd,
                                            weak_factory_.GetWeakPtr()))));

  input_holder_ = base::MakeRefCounted<base::RefCountedData<base::File>>(
      std::move(result.input_write));
  for (auto& buffered : pending_input_) {
    WriteStdin(buffered);
  }
  pending_input_.clear();

  if (kill_requested_) {
    TerminateTree();
  }
  exit_watcher_.StartWatchingOnce(process_.Handle(), this);
}

void PtyChildProcess::OnReadChunk(std::vector<uint8_t> chunk) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (chunk.empty()) {
    return;
  }
  pending_output_.insert(pending_output_.end(), chunk.begin(), chunk.end());
  if (pending_output_.size() >= kFlushBytes) {
    FlushOutput();
    return;
  }
  if (!flush_timer_.IsRunning()) {
    flush_timer_.Start(FROM_HERE, kFlushDelay,
                       base::BindOnce(&PtyChildProcess::FlushOutput,
                                      weak_factory_.GetWeakPtr()));
  }
}

void PtyChildProcess::FlushOutput() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  flush_timer_.Stop();
  if (pending_output_.empty()) {
    return;
  }
  std::vector<uint8_t> out;
  out.swap(pending_output_);
  if (client_) {
    client_->OnStdout(std::move(out));
  }
}

void PtyChildProcess::OnReadEnd() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  read_ended_ = true;
  if (closing_) {
    return;
  }
  if (!exited_) {
    // The console host went away under a live child: the terminal is gone,
    // so the tree goes too; the exit watcher finishes the teardown.
    TerminateTree();
    return;
  }
  if (!close_begun_) {
    BeginClose();
  } else if (console_closed_) {
    FinishExit();
  }
}

void PtyChildProcess::OnObjectSignaled(HANDLE /*object*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  exited_ = true;
  DWORD code = 0;
  if (process_.IsValid() && ::GetExitCodeProcess(process_.Handle(), &code)) {
    exit_code_ = static_cast<int>(code);
  }
  if (read_ended_) {
    BeginClose();
    return;
  }
  drain_timer_.Start(FROM_HERE, kExitDrainTimeout,
                     base::BindOnce(&PtyChildProcess::BeginClose,
                                    weak_factory_.GetWeakPtr()));
}

void PtyChildProcess::BeginClose() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (close_begun_) {
    return;
  }
  close_begun_ = true;
  drain_timer_.Stop();
  // Whatever the shell left running dies with the terminal.
  TerminateTree();
  if (!console_) {
    OnConsoleClosed();
    return;
  }
  close_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&PtyChildProcess::CloseConsoleOnWorker, table_,
                     std::exchange(console_, nullptr)),
      base::BindOnce(&PtyChildProcess::OnConsoleClosed,
                     weak_factory_.GetWeakPtr()));
}

void PtyChildProcess::OnConsoleClosed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  console_closed_ = true;
  if (read_ended_) {
    FinishExit();
    return;
  }
  // The pipe breaks once the console host exits; a read still blocked after
  // that is cancelled so the sequence is released.
  drain_timer_.Start(
      FROM_HERE, kReadEndTimeout,
      base::BindOnce(
          [](base::WeakPtr<PtyChildProcess> self) {
            if (!self) {
              return;
            }
            if (self->reader_) {
              self->reader_->CancelRead();
            }
            self->FinishExit();
          },
          weak_factory_.GetWeakPtr()));
}

void PtyChildProcess::FinishExit() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closing_) {
    return;
  }
  drain_timer_.Stop();
  FlushOutput();
  if (client_) {
    auto exit = mojom::ShellExitInfo::New();
    // A killed child reports no code (the terminate status is not the
    // program's), the same as a killed pipe child.
    exit->code = kill_requested_ ? std::nullopt : exit_code_;
    exit->killed = kill_requested_ || !exit_code_.has_value();
    exit->timed_out = false;
    exit->duration_ms = static_cast<uint64_t>(
        (base::TimeTicks::Now() - start_time_).InMilliseconds());
    client_->OnExit(std::move(exit));
  }
  RequestClose();
}

void PtyChildProcess::TerminateTree() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (job_.is_valid()) {
    ::TerminateJobObject(job_.get(), 1);
  } else if (process_.IsValid()) {
    process_.Terminate(1, /*wait=*/false);
  }
}

void PtyChildProcess::WriteStdin(const std::vector<uint8_t>& data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closing_ || data.empty()) {
    return;
  }
  if (!launched_) {
    pending_input_.push_back(data);
    return;
  }
  // Off the binding sequence: a write blocks while the console's input
  // buffer is full.
  writer_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<base::File>> holder,
             std::vector<uint8_t> bytes) {
            if (holder->data.IsValid()) {
              holder->data.WriteAtCurrentPos(base::span(bytes));
            }
          },
          input_holder_, data));
}

void PtyChildProcess::CloseStdin() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void PtyChildProcess::Kill() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (kill_requested_) {
    return;
  }
  kill_requested_ = true;
  if (launched_) {
    TerminateTree();
  }
  // The exit watcher observes the death and runs the close path.
}

void PtyChildProcess::Resize(mojom::PtySizePtr size) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!size || size->cols < 1 || size->cols > kMaxCells || size->rows < 1 ||
      size->rows > kMaxCells) {
    return;
  }
  if (!console_ || !table_.resize) {
    return;
  }
  const COORD coord{static_cast<SHORT>(size->cols),
                    static_cast<SHORT>(size->rows)};
  const HRESULT hr = table_.resize(console_, coord);
  if (FAILED(hr)) {
    LOG(WARNING) << "[PtyChildProcess] ResizePseudoConsole failed: 0x"
                 << std::hex << hr;
  }
}

void PtyChildProcess::OnControllerDisconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Kill();
  RequestClose();
}

void PtyChildProcess::OnClientDisconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Kill();
  RequestClose();
}

void PtyChildProcess::RequestClose() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closing_) {
    return;
  }
  closing_ = true;
  flush_timer_.Stop();
  drain_timer_.Stop();
  controller_.reset();
  client_.reset();
  if (on_closed_) {
    std::move(on_closed_).Run();
  }
}

}  // namespace nexus
