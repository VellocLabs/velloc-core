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
#include "base/process/launch.h"
#include "base/time/time.h"

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
ChildProcessOutcome RunChildProcess(const ChildProcessRequest& request);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_CHILD_PROCESS_RUNNER_H_
