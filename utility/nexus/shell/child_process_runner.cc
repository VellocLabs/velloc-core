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

#include "base/win/scoped_handle.h"
#elif BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace nexus {

namespace {

#if BUILDFLAG(IS_WIN)

// Slice 5 hardening: every spawned child is placed in a Job Object that
// (a) kills the entire process tree when the JO handle closes
// (KILL_ON_JOB_CLOSE) and (b) lets us call TerminateJobObject to nuke
// every descendant in one syscall on timeout. Without this, processes
// the child re-parents (via DETACHED_PROCESS or grandchildren that have
// already exec'd) survive the parent's death and leak into the host.
//
// The Job Object is created with no quota limits and only KILL_ON_JOB_CLOSE
// + BREAKAWAY_OK disabled, so the child cannot escape via CreateProcess
// flags. Resource caps (memory, CPU) are intentionally *not* set here —
// the SafeCommandPolicy clamps timeout / output bytes upstream, and
// killing on timeout is enforced via TerminateJobObject below.
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

#endif  // BUILDFLAG(IS_WIN)

// Reads `path` into `out`, capping at `max_bytes`. Sets `*truncated`
// when the file is bigger than the cap. Returns false only on I/O error
// — an empty file is success with `out->empty()`.
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

  const uint32_t to_read = static_cast<uint32_t>(
      std::min<int64_t>(*file_size, static_cast<int64_t>(max_bytes)));
  out->resize(to_read);
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    out->clear();
    return false;
  }
  // Read the prefix; any tail beyond `max_bytes` is silently dropped.
  std::optional<size_t> read = file.Read(0, *out);
  if (!read.has_value()) {
    out->clear();
    return false;
  }
  out->resize(*read);
  if (static_cast<int64_t>(*read) < *file_size) {
    *truncated = true;
  }
  return true;
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

ChildProcessOutcome::ChildProcessOutcome() = default;

ChildProcessOutcome::~ChildProcessOutcome() = default;

ChildProcessOutcome::ChildProcessOutcome(const ChildProcessOutcome&) = default;

ChildProcessOutcome& ChildProcessOutcome::operator=(
    const ChildProcessOutcome&) = default;

ChildProcessOutcome::ChildProcessOutcome(ChildProcessOutcome&&) = default;

ChildProcessOutcome& ChildProcessOutcome::operator=(ChildProcessOutcome&&) =
    default;

ChildProcessOutcome RunChildProcess(const ChildProcessRequest& request) {
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

  base::CommandLine cmdline(request.exe);
  for (const std::string& arg : request.args) {
    cmdline.AppendArg(arg);
  }

  base::LaunchOptions options;
  options.current_directory = request.cwd;
  options.environment = request.env;
  options.clear_environment = true;

#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle stdin_handle = OpenNulForChildRead();
  base::win::ScopedHandle stdout_handle =
      OpenWindowsFileForChildWrite(stdout_path);
  base::win::ScopedHandle stderr_handle =
      OpenWindowsFileForChildWrite(stderr_path);
  if (!stdin_handle.is_valid() || !stdout_handle.is_valid() ||
      !stderr_handle.is_valid()) {
    base::DeleteFile(stdout_path);
    base::DeleteFile(stderr_path);
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
  base::ScopedFD stdin_fd = OpenDevNullForChildRead();
  base::ScopedFD stdout_fd = OpenPosixFileForChildWrite(stdout_path);
  base::ScopedFD stderr_fd = OpenPosixFileForChildWrite(stderr_path);
  if (!stdin_fd.is_valid() || !stdout_fd.is_valid() ||
      !stderr_fd.is_valid()) {
    base::DeleteFile(stdout_path);
    base::DeleteFile(stderr_path);
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
  base::Process process = base::LaunchProcess(cmdline, options);

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
    outcome.launched = false;
    outcome.launch_error_code = "LAUNCH_FAILED";
    outcome.launch_error_message =
        "base::LaunchProcess failed for " + request.exe.AsUTF8Unsafe();
    return outcome;
  }

  outcome.launched = true;

  int exit_code = -1;
  const bool exited_normally =
      process.WaitForExitWithTimeout(request.timeout, &exit_code);

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

  return outcome;
}

}  // namespace nexus
