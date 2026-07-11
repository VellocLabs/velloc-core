// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_CHILD_PROCESS_RUNNER_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_CHILD_PROCESS_RUNNER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/scoped_handle.h"
#endif

namespace nexus {

// One-shot child-process invocation request. All fields are pre-validated
// and pre-sanitised by `SafeCommandPolicy` before they reach the runner;
// the runner does no policy checks of its own.
struct ChildProcessRequest {
  ChildProcessRequest();
  ~ChildProcessRequest();
  ChildProcessRequest(const ChildProcessRequest&);
  ChildProcessRequest& operator=(const ChildProcessRequest&);
  ChildProcessRequest(ChildProcessRequest&&);
  ChildProcessRequest& operator=(ChildProcessRequest&&);

  // Absolute path to argv[0]. The runner does NOT consult PATH.
  base::FilePath exe;

  // argv excluding argv[0].
  std::vector<std::string> args;

  // Working directory. Must already be validated by the caller.
  base::FilePath cwd;

  // Full env map. Used as-is with `clear_environment = true`, so missing
  // PATH or HOME means the child won't see them.
  base::EnvironmentMap env;

  // When true, the child inherits the parent (utility-process) environment
  // instead of running with the cleared `env` above. Used by the lightweight
  // `--version` capability probe, which needs the host PATH / DLL search
  // paths just like the `base::GetAppOutput` call it replaced. The Exec path
  // leaves this false and relies on the curated `env`.
  bool inherit_environment = false;

  // Bytes written to the child's stdin (then the stream is closed). Empty
  // means the child gets an empty stdin (NUL / /dev/null), preserving the
  // prior behaviour. Delivered via a temp file the child reads from — same
  // pattern as the stdout/stderr capture files, just opened read-only — so
  // the parent never has to pump a pipe concurrently with the wait.
  std::string stdin_data;

  // Wall-clock timeout. Already clamped by `SafeCommandPolicy`.
  base::TimeDelta timeout;

  // Per-stream output cap. Already clamped by `SafeCommandPolicy`.
  uint32_t max_output_bytes = 0;
};

// Fully-formed result of a `RunChildProcess` call. The runner never
// returns a "couldn't even launch" failure mid-result: callers that need
// to surface a launch error use the `launch_error_*` fields and ignore
// the rest.
struct ChildProcessOutcome {
  ChildProcessOutcome();
  ~ChildProcessOutcome();
  ChildProcessOutcome(const ChildProcessOutcome&);
  ChildProcessOutcome& operator=(const ChildProcessOutcome&);
  ChildProcessOutcome(ChildProcessOutcome&&);
  ChildProcessOutcome& operator=(ChildProcessOutcome&&);

  // True iff `base::LaunchProcess` returned a valid Process. When false,
  // the `launch_error_*` fields are populated and everything else is
  // default-initialised.
  bool launched = false;
  std::string launch_error_code;       // e.g. "LAUNCH_FAILED"
  std::string launch_error_message;

  std::vector<uint8_t> stdout_bytes;
  std::vector<uint8_t> stderr_bytes;
  bool stdout_truncated = false;
  bool stderr_truncated = false;

  // Exit information. `code` and `signal` follow `base::Process`
  // conventions: on POSIX one of them is set, on Windows only `code` is
  // ever set.
  std::optional<int32_t> code;
  std::optional<int32_t> signal;

  // True iff the runner killed the child because `timeout` expired.
  bool timed_out = false;
  // True whenever the runner forcibly terminated the child (timeout
  // expired, or any other internal kill reason). `timed_out` implies
  // `killed`; the inverse does not hold.
  bool killed = false;

  // Wall-clock time from spawn to the runner observing exit (or kill).
  base::TimeDelta duration;
};

// Handle needed for out-of-band tree termination of a background child.
// Windows stores the Job Object so explicit Kill can terminate every descendant;
// POSIX stores the process-group id created by `new_process_group`.
struct ChildProcessKillHandle {
  ChildProcessKillHandle();
  ~ChildProcessKillHandle();
  ChildProcessKillHandle(ChildProcessKillHandle&&);
  ChildProcessKillHandle& operator=(ChildProcessKillHandle&&);

  ChildProcessKillHandle(const ChildProcessKillHandle&) = delete;
  ChildProcessKillHandle& operator=(const ChildProcessKillHandle&) = delete;

  bool IsValid() const;

#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle job;
  base::Process process;
#else
  base::ProcessId process_group_id = base::kNullProcessId;
#endif
};

// Live output snapshot callback for background callers. The runner invokes it
// on the worker thread while the child is still running, carrying the capped
// stdout/stderr bytes captured so far (not a delta). Callers that need a
// sequence-affine sink should wrap it with base::BindPostTask.
using ChildProcessOutputCallback = base::RepeatingCallback<void(
    bool is_stdout,
    std::vector<uint8_t> bytes,
    bool truncated)>;

// Synchronous, blocking child-process invocation. Must run on a
// `base::MayBlock()` thread; never call from the Mojo binding sequence.
//
// Captures stdout and stderr to platform-native temp files (one per
// stream) so the parent never has to pump pipes concurrently. After the
// child exits or is killed, the runner reads back up to
// `max_output_bytes` per stream from the temp files and deletes them.
//
// The runner's contract:
//   * Either fully populates `ChildProcessOutcome::launched = true` and
//     all the result fields (exit, output, duration), OR
//   * Sets `launched = false` with `launch_error_code` /
//     `launch_error_message` describing why no child ever ran.
//
// `on_launched`, when non-null, is invoked ON THIS WORKER THREAD the instant
// the child launches (before the blocking wait) with a kill handle for the
// child tree. A background caller (`Spawn`) bounces that handle to its own
// sequence so it can later terminate the still-running tree via `Kill`. Null
// (the default) for foreground / probe callers that never kill out-of-band.
ChildProcessOutcome RunChildProcess(
    const ChildProcessRequest& request,
    base::OnceCallback<void(ChildProcessKillHandle)> on_launched =
        base::NullCallback());

ChildProcessOutcome RunChildProcessWithOutput(
    const ChildProcessRequest& request,
    base::OnceCallback<void(ChildProcessKillHandle)> on_launched,
    ChildProcessOutputCallback on_output);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_CHILD_PROCESS_RUNNER_H_
