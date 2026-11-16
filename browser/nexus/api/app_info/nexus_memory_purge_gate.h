// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_MEMORY_PURGE_GATE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_MEMORY_PURGE_GATE_H_

#include "base/time/time.h"

namespace base {
class CommandLine;
}

namespace nexus {

// Contract: when the host runs `appInfo.purgeRenderCaches` at all (P-3). A
// release build always may; a debug build only when started with
// `--velloc-memory-purge-for-testing`, which also lifts the startup grace.
// Everything here is UI-thread safe and side-effect free.

inline constexpr char kMemoryPurgeForTestingSwitch[] =
    "velloc-memory-purge-for-testing";

// No purge before this long after the browser process started.
inline constexpr base::TimeDelta kMemoryPurgeStartupGrace = base::Seconds(60);

// Whether this build, started with `command_line`, offers the purge.
bool MemoryPurgeEnabled(bool release_build,
                        const base::CommandLine& command_line);

// Whether the testing switch is honoured: a debug build that has it.
bool MemoryPurgeForTesting(bool release_build,
                           const base::CommandLine& command_line);

// Whether `now` is inside the startup grace of a process started at
// `process_start`. A null start, or a clock that went backwards, counts as
// inside it.
bool InMemoryPurgeStartupGrace(base::Time process_start, base::Time now);

// The two above for this process.
bool MemoryPurgeEnabledInThisProcess();
bool MemoryPurgeForTestingInThisProcess();
bool InMemoryPurgeStartupGraceNow();

// Whether the foreground window belongs to this process; false where the
// platform cannot tell.
bool AppWindowInForeground();

// Test seam: while one is alive, this process counts as offering the purge and
// as outside the startup grace, whatever the build and the process age.
class ScopedMemoryPurgeGateOpenForTesting {
 public:
  ScopedMemoryPurgeGateOpenForTesting();
  ScopedMemoryPurgeGateOpenForTesting(
      const ScopedMemoryPurgeGateOpenForTesting&) = delete;
  ScopedMemoryPurgeGateOpenForTesting& operator=(
      const ScopedMemoryPurgeGateOpenForTesting&) = delete;
  ~ScopedMemoryPurgeGateOpenForTesting();
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_MEMORY_PURGE_GATE_H_
