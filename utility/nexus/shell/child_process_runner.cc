// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/child_process_runner.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/process/environment_internal.h"
#include "base/win/scoped_handle.h"
#include "base/win/scoped_process_information.h"
#include "base/win/startup_information.h"
#elif BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace nexus {

namespace {

#if BUILDFLAG(IS_WIN)

// Security: every child goes in a Job Object with KILL_ON_JOB_CLOSE and no
// breakaway, so the whole tree dies with the handle or on TerminateJobObject.
// No resource caps; SafeCommandPolicy clamps timeout and output upstream.
base::win::ScopedHandle CreateKillOnCloseJobObject() {
  HANDLE raw = ::CreateJobObjectW(/*lpJobAttributes=*/nullptr,
                                  /*lpName=*/nullptr);
  if (raw == nullptr) {
    PLOG(WARNING) << "[NexusShellService] CreateJobObjectW failed";
    return base::win::ScopedHandle();
  }
  base::win::ScopedHandle job(raw);

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
      JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
    PLOG(WARNING) << "[NexusShellService] SetInformationJobObject failed";
    return base::win::ScopedHandle();
  }
  return job;
}

bool AssignProcessToJob(HANDLE job, HANDLE proc) {
  if (!::AssignProcessToJobObject(job, proc)) {
    PLOG(WARNING)
        << "[NexusShellService] AssignProcessToJobObject failed; descendants "
           "of the spawned process will not be cleaned up on timeout.";
    return false;
  }
  return true;
}

// Opens an inheritable file handle for writing. Returns a null
// ScopedHandle on failure. Used for redirecting child stdout/stderr to a
// temp file we'll read back after the process exits.
base::win::ScopedHandle OpenWindowsFileForChildWrite(
    const base::FilePath& path) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE raw = ::CreateFileW(
      path.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return base::win::ScopedHandle();
  }
  return base::win::ScopedHandle(raw);
}

base::win::ScopedHandle OpenNulForChildRead() {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE raw = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                             OPEN_EXISTING, 0, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return base::win::ScopedHandle();
  }
  return base::win::ScopedHandle(raw);
}

// Opens an inheritable read handle on an existing temp file. Used to feed
// `ChildProcessRequest::stdin_data` (already written to that file) into the
// child's stdin. Mirrors OpenWindowsFileForChildWrite but read-only.
base::win::ScopedHandle OpenWindowsFileForChildRead(
    const base::FilePath& path) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE raw = ::CreateFileW(
      path.value().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &sa, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return base::win::ScopedHandle();
  }
  return base::win::ScopedHandle(raw);
}

// Launches the child with CREATE_NO_WINDOW (base::LaunchOptions has no
// creation flags; SW_HIDE still flickers) by reproducing base's CreateProcess
// path. `env` is the whole environment unless `inherit_environment`. Returns
// an invalid Process on failure; the caller keeps the stdio handles.
base::Process LaunchChildWithoutWindow(const base::CommandLine& cmdline,
                                       const base::FilePath& cwd,
                                       const base::EnvironmentMap& env,
                                       bool inherit_environment,
                                       HANDLE stdin_handle,
                                       HANDLE stdout_handle,
                                       HANDLE stderr_handle) {
  std::vector<HANDLE> handles_to_inherit = {stdin_handle, stdout_handle,
                                            stderr_handle};

  base::win::StartupInformation startup_info_wrapper;
  STARTUPINFOW* startup_info = startup_info_wrapper.startup_info();

  // One extended attribute: the explicit handle-inheritance list, so the
  // child inherits only our three stdio handles and nothing else.
  if (!startup_info_wrapper.InitializeProcThreadAttributeList(
          /*attribute_count=*/1)) {
    PLOG(ERROR) << "[NexusShellService] InitializeProcThreadAttributeList "
                   "failed";
    return base::Process();
  }
  for (HANDLE handle : handles_to_inherit) {
    ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }
  if (!startup_info_wrapper.UpdateProcThreadAttribute(
          PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_to_inherit.data(),
          handles_to_inherit.size() * sizeof(HANDLE))) {
    PLOG(ERROR) << "[NexusShellService] UpdateProcThreadAttribute failed";
    return base::Process();
  }

  startup_info->dwFlags |= STARTF_USESHOWWINDOW;
  startup_info->wShowWindow = SW_HIDE;
  startup_info->dwFlags |= STARTF_USESTDHANDLES;
  startup_info->hStdInput = stdin_handle;
  startup_info->hStdOutput = stdout_handle;
  startup_info->hStdError = stderr_handle;

  // Clear the parent's environment and apply `env`, mirroring
  // base::LaunchProcess with clear_environment = true. When inheriting, pass a
  // null environment block so the child gets the parent's environment.
  DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
  base::NativeEnvironmentString env_block;
  LPVOID environment = nullptr;
  if (!inherit_environment) {
    static const wchar_t kEmptyEnvironment[] = {0};
    env_block = base::internal::AlterEnvironment(kEmptyEnvironment, env);
    environment = env_block.data();
    flags |= CREATE_UNICODE_ENVIRONMENT;
  }

  // CreateProcessW may write to lpCommandLine, so hand it a mutable buffer.
  std::wstring writable_cmdline = cmdline.GetCommandLineString();

  LPCWSTR current_directory = cwd.empty() ? nullptr : cwd.value().c_str();

  PROCESS_INFORMATION temp_process_info = {};
  if (!::CreateProcessW(/*lpApplicationName=*/nullptr, writable_cmdline.data(),
                        /*lpProcessAttributes=*/nullptr,
                        /*lpThreadAttributes=*/nullptr,
                        /*bInheritHandles=*/TRUE, flags, environment,
                        current_directory, startup_info, &temp_process_info)) {
    PLOG(ERROR) << "[NexusShellService] CreateProcessW failed";
    return base::Process();
  }
  base::win::ScopedProcessInformation process_info(temp_process_info);
  return base::Process(process_info.TakeProcessHandle());
}

#elif BUILDFLAG(IS_POSIX)

// Opens a writable, inheritable fd. The caller is responsible for
// putting this fd into LaunchOptions::fds_to_remap; the fd is then
// dup2()'d into the child's STDOUT_FILENO / STDERR_FILENO at fork time.
base::ScopedFD OpenPosixFileForChildWrite(const base::FilePath& path) {
  // Intentionally do NOT pass O_CLOEXEC: the fd must survive
  // base::LaunchProcess's pre-exec phase. fds_to_remap will dup2 it into
  // the right slot regardless, but on some libcs the dup target inherits
  // CLOEXEC from the source.
  int fd = ::open(path.value().c_str(),
                  O_CREAT | O_WRONLY | O_TRUNC, 0600);
  if (fd < 0) {
    return base::ScopedFD();
  }
  return base::ScopedFD(fd);
}

base::ScopedFD OpenDevNullForChildRead() {
  int fd = ::open("/dev/null", O_RDONLY);
  if (fd < 0) {
    return base::ScopedFD();
  }
  return base::ScopedFD(fd);
}

// Opens a read-only, inheritable fd on an existing temp file holding the
// child's stdin bytes. Mirrors OpenPosixFileForChildWrite but read-only.
base::ScopedFD OpenPosixFileForChildRead(const base::FilePath& path) {
  int fd = ::open(path.value().c_str(), O_RDONLY);
  if (fd < 0) {
    return base::ScopedFD();
  }
  return base::ScopedFD(fd);
}

#endif  // BUILDFLAG(IS_WIN)

// Reads the tail of `path` (shell output's verdict is at the end), capped at
// `max_bytes`, setting `*truncated` when cut. False only on I/O error. A
// leading partial UTF-8 sequence is normalized browser-side.
bool ReadCapped(const base::FilePath& path,
                uint32_t max_bytes,
                std::vector<uint8_t>* out,
                bool* truncated) {
  out->clear();
  *truncated = false;

  std::optional<int64_t> file_size = base::GetFileSize(path);
  if (!file_size.has_value()) {
    return false;
  }
  if (*file_size == 0) {
    return true;
  }

  const int64_t offset =
      std::max<int64_t>(0, *file_size - static_cast<int64_t>(max_bytes));
  const uint32_t to_read = static_cast<uint32_t>(*file_size - offset);
  out->resize(to_read);
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    out->clear();
    return false;
  }
  // Read the last `to_read` bytes; anything before `offset` is dropped and
  // reported via `*truncated` so callers can surface the elision.
  std::optional<size_t> read = file.Read(offset, *out);
  if (!read.has_value()) {
    out->clear();
    return false;
  }
  out->resize(*read);
  if (offset > 0 || static_cast<int64_t>(*read) < to_read) {
    *truncated = true;
  }
  return true;
}

constexpr int kOutputSnapshotIntervalMs = 250;

void EmitOutputSnapshot(const base::FilePath& stdout_path,
                        const base::FilePath& stderr_path,
                        uint32_t max_output_bytes,
                        const ChildProcessOutputCallback& on_output) {
  if (on_output.is_null()) {
    return;
  }

  std::vector<uint8_t> bytes;
  bool truncated = false;
  if (ReadCapped(stdout_path, max_output_bytes, &bytes, &truncated)) {
    on_output.Run(/*is_stdout=*/true, std::move(bytes), truncated);
  }
  if (ReadCapped(stderr_path, max_output_bytes, &bytes, &truncated)) {
    on_output.Run(/*is_stdout=*/false, std::move(bytes), truncated);
  }
}

}  // namespace

ChildProcessRequest::ChildProcessRequest() = default;

ChildProcessRequest::~ChildProcessRequest() = default;

ChildProcessRequest::ChildProcessRequest(const ChildProcessRequest&) = default;

ChildProcessRequest& ChildProcessRequest::operator=(
    const ChildProcessRequest&) = default;

ChildProcessRequest::ChildProcessRequest(ChildProcessRequest&&) = default;

ChildProcessRequest& ChildProcessRequest::operator=(ChildProcessRequest&&) =
    default;

ChildProcessKillHandle::ChildProcessKillHandle() = default;

ChildProcessKillHandle::~ChildProcessKillHandle() = default;

ChildProcessKillHandle::ChildProcessKillHandle(ChildProcessKillHandle&&) =
    default;

ChildProcessKillHandle& ChildProcessKillHandle::operator=(
    ChildProcessKillHandle&&) = default;

bool ChildProcessKillHandle::IsValid() const {
#if BUILDFLAG(IS_WIN)
  return job.is_valid() || process.IsValid();
#else
  return process_group_id != base::kNullProcessId;
#endif
}

ChildProcessOutcome::ChildProcessOutcome() = default;

ChildProcessOutcome::~ChildProcessOutcome() = default;

ChildProcessOutcome::ChildProcessOutcome(const ChildProcessOutcome&) = default;

ChildProcessOutcome& ChildProcessOutcome::operator=(
    const ChildProcessOutcome&) = default;

ChildProcessOutcome::ChildProcessOutcome(ChildProcessOutcome&&) = default;

ChildProcessOutcome& ChildProcessOutcome::operator=(ChildProcessOutcome&&) =
    default;

ChildProcessOutcome RunChildProcess(
    const ChildProcessRequest& request,
    base::OnceCallback<void(ChildProcessKillHandle)> on_launched) {
  return RunChildProcessWithOutput(request, std::move(on_launched),
                                   ChildProcessOutputCallback());
}

ChildProcessOutcome RunChildProcessWithOutput(
    const ChildProcessRequest& request,
    base::OnceCallback<void(ChildProcessKillHandle)> on_launched,
    ChildProcessOutputCallback on_output) {
  ChildProcessOutcome outcome;

  // Set up two temp files for stdout/stderr capture. We delete them at
  // the end of this function regardless of outcome.
  base::FilePath stdout_path;
  base::FilePath stderr_path;
  if (!base::CreateTemporaryFile(&stdout_path) ||
      !base::CreateTemporaryFile(&stderr_path)) {
    outcome.launched = false;
    outcome.launch_error_code = "LAUNCH_FAILED";
    outcome.launch_error_message =
        "Could not create temp files for child stdout/stderr capture";
    return outcome;
  }

  // Optional stdin: write the request bytes to a temp file the child reads
  // from. Empty `stdin_data` leaves `stdin_path` empty, preserving the
  // NUL / /dev/null behaviour below. We delete `stdin_path` on every exit
  // path alongside the stdout/stderr temp files.
  base::FilePath stdin_path;
  if (!request.stdin_data.empty()) {
    if (!base::CreateTemporaryFile(&stdin_path) ||
        !base::WriteFile(stdin_path, request.stdin_data)) {
      base::DeleteFile(stdout_path);
      base::DeleteFile(stderr_path);
      if (!stdin_path.empty()) {
        base::DeleteFile(stdin_path);
      }
      outcome.launched = false;
      outcome.launch_error_code = "LAUNCH_FAILED";
      outcome.launch_error_message =
          "Could not create temp file for child stdin";
      return outcome;
    }
  }

  base::CommandLine cmdline(request.exe);
  for (const std::string& arg : request.args) {
    cmdline.AppendArg(arg);
  }

  base::LaunchOptions options;
  options.current_directory = request.cwd;
  options.environment = request.env;
  // Inherit-environment requests (the `--version` probe) leave the parent
  // environment intact; everything else runs with a cleared, curated env.
  options.clear_environment = !request.inherit_environment;

#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle stdin_handle =
      stdin_path.empty() ? OpenNulForChildRead()
                         : OpenWindowsFileForChildRead(stdin_path);
  base::win::ScopedHandle stdout_handle =
      OpenWindowsFileForChildWrite(stdout_path);
  base::win::ScopedHandle stderr_handle =
      OpenWindowsFileForChildWrite(stderr_path);
  if (!stdin_handle.is_valid() || !stdout_handle.is_valid() ||
      !stderr_handle.is_valid()) {
    base::DeleteFile(stdout_path);
    base::DeleteFile(stderr_path);
    if (!stdin_path.empty()) {
      base::DeleteFile(stdin_path);
    }
    outcome.launched = false;
    outcome.launch_error_code = "LAUNCH_FAILED";
    outcome.launch_error_message =
        "Could not open NUL / temp file handles for child";
    return outcome;
  }

  options.start_hidden = true;
  options.stdin_handle = stdin_handle.get();
  options.stdout_handle = stdout_handle.get();
  options.stderr_handle = stderr_handle.get();
  options.handles_to_inherit.push_back(stdin_handle.get());
  options.handles_to_inherit.push_back(stdout_handle.get());
  options.handles_to_inherit.push_back(stderr_handle.get());
#elif BUILDFLAG(IS_POSIX)
  base::ScopedFD stdin_fd =
      stdin_path.empty() ? OpenDevNullForChildRead()
                         : OpenPosixFileForChildRead(stdin_path);
  base::ScopedFD stdout_fd = OpenPosixFileForChildWrite(stdout_path);
  base::ScopedFD stderr_fd = OpenPosixFileForChildWrite(stderr_path);
  if (!stdin_fd.is_valid() || !stdout_fd.is_valid() ||
      !stderr_fd.is_valid()) {
    base::DeleteFile(stdout_path);
    base::DeleteFile(stderr_path);
    if (!stdin_path.empty()) {
      base::DeleteFile(stdin_path);
    }
    outcome.launched = false;
    outcome.launch_error_code = "LAUNCH_FAILED";
    outcome.launch_error_message =
        "Could not open /dev/null / temp file fds for child";
    return outcome;
  }

  options.fds_to_remap.emplace_back(stdin_fd.get(), STDIN_FILENO);
  options.fds_to_remap.emplace_back(stdout_fd.get(), STDOUT_FILENO);
  options.fds_to_remap.emplace_back(stderr_fd.get(), STDERR_FILENO);
  // Slice 5 hardening: place the child in its own process group so that
  // on timeout we can `kill(-pgid, SIGKILL)` to take down any
  // grandchildren the child has already forked (shells, build systems,
  // wrappers around `node`, etc.). Without this, only the immediate
  // child is reaped and orphaned descendants reattach to init.
  options.new_process_group = true;
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_WIN)
  // Pre-create the Job Object so we can assign the child to it
  // immediately after launch. If creation fails we still launch the
  // child but lose tree-kill semantics — log and continue.
  base::win::ScopedHandle job = CreateKillOnCloseJobObject();
#endif

  const base::TimeTicks spawn_start = base::TimeTicks::Now();
#if BUILDFLAG(IS_WIN)
  // Launch via CREATE_NO_WINDOW (see LaunchChildWithoutWindow) so the console
  // child never flashes a window. `options.start_hidden`/handles above are
  // unused on this path but left intact to document the intended stdio wiring
  // and keep the POSIX branch (which does use `options`) unchanged.
  base::Process process = LaunchChildWithoutWindow(
      cmdline, request.cwd, request.env, request.inherit_environment,
      stdin_handle.get(), stdout_handle.get(), stderr_handle.get());
#else
  base::Process process = base::LaunchProcess(cmdline, options);
#endif

#if BUILDFLAG(IS_WIN)
  // The handles have been duplicated into the child by the OS at this
  // point; releasing our copies prevents them from holding the temp
  // files open after the child closes them.
  stdin_handle.Close();
  stdout_handle.Close();
  stderr_handle.Close();

  // Assign the freshly-launched child to the Job Object. There is a
  // small window between LaunchProcess returning and AssignProcessToJob
  // succeeding during which a fast-exiting child may already have
  // spawned a grandchild; that grandchild will not be in the JO.
  // Mitigated in practice by the fact that we don't have a CREATE_SUSPENDED
  // codepath in base::LaunchProcess yet — accept the race for now and
  // log so it's diagnosable from the wild.
  bool job_assigned = false;
  if (process.IsValid() && job.is_valid()) {
    job_assigned = AssignProcessToJob(job.get(), process.Handle());
  }
#endif

  if (!process.IsValid()) {
    base::DeleteFile(stdout_path);
    base::DeleteFile(stderr_path);
    if (!stdin_path.empty()) {
      base::DeleteFile(stdin_path);
    }
    outcome.launched = false;
    outcome.launch_error_code = "LAUNCH_FAILED";
    outcome.launch_error_message =
        "Failed to launch child process " + request.exe.AsUTF8Unsafe();
    return outcome;
  }

  outcome.launched = true;

  // Hand a tree-kill handle to the caller before we block on the wait, so a
  // background `Spawn` can publish it to its sequence and terminate this child
  // tree later (the `Kill` path). This mirrors the timeout path's tree-kill
  // semantics: Job Object on Windows, process group on POSIX.
  if (on_launched) {
    ChildProcessKillHandle kill_handle;
#if BUILDFLAG(IS_WIN)
    if (job_assigned && job.is_valid()) {
      HANDLE duplicated_job = nullptr;
      if (::DuplicateHandle(::GetCurrentProcess(), job.get(),
                            ::GetCurrentProcess(), &duplicated_job,
                            /*dwDesiredAccess=*/0, /*bInheritHandle=*/FALSE,
                            DUPLICATE_SAME_ACCESS)) {
        kill_handle.job.Set(duplicated_job);
      } else {
        PLOG(WARNING) << "[NexusShellService] DuplicateHandle(job) failed; "
                         "explicit background Kill will fall back to child-only "
                         "termination";
      }
    }
    kill_handle.process = process.Duplicate();
#else
    kill_handle.process_group_id = process.Pid();
#endif
    std::move(on_launched).Run(std::move(kill_handle));
  }

  int exit_code = -1;
  bool exited_normally = false;
  if (on_output.is_null()) {
    exited_normally = process.WaitForExitWithTimeout(request.timeout, &exit_code);
  } else {
    while (true) {
      const base::TimeDelta elapsed = base::TimeTicks::Now() - spawn_start;
      if (elapsed >= request.timeout) {
        exited_normally = process.WaitForExitWithTimeout(base::Milliseconds(0),
                                                         &exit_code);
        EmitOutputSnapshot(stdout_path, stderr_path, request.max_output_bytes,
                           on_output);
        break;
      }
      const base::TimeDelta remaining = request.timeout - elapsed;
      const base::TimeDelta wait =
          std::min(remaining, base::Milliseconds(kOutputSnapshotIntervalMs));
      exited_normally = process.WaitForExitWithTimeout(wait, &exit_code);
      EmitOutputSnapshot(stdout_path, stderr_path, request.max_output_bytes,
                         on_output);
      if (exited_normally) {
        break;
      }
    }
  }

  if (!exited_normally) {
    // Process did not exit within the timeout. Take down the entire
    // descendant tree, not just the immediate child:
    //   * Windows: TerminateJobObject if we successfully assigned the
    //     child to a Job Object; otherwise fall back to Terminate().
    //   * POSIX: send SIGKILL to the whole process group we created
    //     above via `new_process_group = true`.
#if BUILDFLAG(IS_WIN)
    if (job_assigned) {
      // Best-effort tree-kill. Exit code -1 mirrors what
      // base::Process::Terminate would have done for the child alone.
      ::TerminateJobObject(job.get(), static_cast<UINT>(-1));
    }
    process.Terminate(/*exit_code=*/-1, /*wait=*/true);
#elif BUILDFLAG(IS_POSIX)
    pid_t pgid = process.Pid();
    if (pgid > 0) {
      if (::killpg(pgid, SIGKILL) != 0) {
        PLOG(WARNING) << "[NexusShellService] killpg(" << pgid
                      << ", SIGKILL) failed; falling back to single-process "
                         "Terminate.";
        process.Terminate(/*exit_code=*/-1, /*wait=*/true);
      } else {
        // killpg signalled the group; reap the immediate child so we
        // don't leak a zombie. WaitForExitWithTimeout will return
        // promptly now that the child has been signalled.
        int reaped = -1;
        process.WaitForExitWithTimeout(base::Seconds(2), &reaped);
      }
    } else {
      process.Terminate(/*exit_code=*/-1, /*wait=*/true);
    }
#endif
    outcome.timed_out = true;
    outcome.killed = true;
    outcome.code = std::nullopt;
#if BUILDFLAG(IS_POSIX)
    outcome.signal = 9;  // SIGKILL — matches base::Process::Terminate semantics.
#endif
  } else {
    outcome.code = exit_code;
  }

  outcome.duration = base::TimeTicks::Now() - spawn_start;

  bool stdout_ok = ReadCapped(stdout_path, request.max_output_bytes,
                              &outcome.stdout_bytes, &outcome.stdout_truncated);
  bool stderr_ok = ReadCapped(stderr_path, request.max_output_bytes,
                              &outcome.stderr_bytes, &outcome.stderr_truncated);
  if (!stdout_ok) {
    LOG(WARNING) << "[NexusShellService] Failed to read stdout temp file: "
                 << stdout_path.AsUTF8Unsafe();
  }
  if (!stderr_ok) {
    LOG(WARNING) << "[NexusShellService] Failed to read stderr temp file: "
                 << stderr_path.AsUTF8Unsafe();
  }

  base::DeleteFile(stdout_path);
  base::DeleteFile(stderr_path);
  if (!stdin_path.empty()) {
    base::DeleteFile(stdin_path);
  }

  return outcome;
}

}  // namespace nexus
