// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/app_info/nexus_memory_purge_gate.h"

#include "base/command_line.h"
#include "base/process/process.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace nexus {

namespace {

#if defined(NDEBUG)
constexpr bool kReleaseBuild = true;
#else
constexpr bool kReleaseBuild = false;
#endif

int g_open_for_testing = 0;

}  // namespace

bool MemoryPurgeEnabled(bool release_build,
                        const base::CommandLine& command_line) {
  return release_build || command_line.HasSwitch(kMemoryPurgeForTestingSwitch);
}

bool MemoryPurgeForTesting(bool release_build,
                           const base::CommandLine& command_line) {
  return !release_build &&
         command_line.HasSwitch(kMemoryPurgeForTestingSwitch);
}

bool InMemoryPurgeStartupGrace(base::Time process_start, base::Time now) {
  if (process_start.is_null() || now < process_start) {
    return true;
  }
  return now - process_start < kMemoryPurgeStartupGrace;
}

bool MemoryPurgeEnabledInThisProcess() {
  if (g_open_for_testing > 0) {
    return true;
  }
  return MemoryPurgeEnabled(kReleaseBuild,
                            *base::CommandLine::ForCurrentProcess());
}

bool MemoryPurgeForTestingInThisProcess() {
  return MemoryPurgeForTesting(kReleaseBuild,
                               *base::CommandLine::ForCurrentProcess());
}

bool InMemoryPurgeStartupGraceNow() {
  if (g_open_for_testing > 0 || MemoryPurgeForTestingInThisProcess()) {
    return false;
  }
  return InMemoryPurgeStartupGrace(base::Process::Current().CreationTime(),
                                   base::Time::Now());
}

bool AppWindowInForeground() {
#if BUILDFLAG(IS_WIN)
  HWND foreground = ::GetForegroundWindow();
  if (!foreground) {
    return false;
  }
  DWORD owner = 0;
  ::GetWindowThreadProcessId(foreground, &owner);
  return owner == ::GetCurrentProcessId();
#else
  return false;
#endif
}

ScopedMemoryPurgeGateOpenForTesting::ScopedMemoryPurgeGateOpenForTesting() {
  ++g_open_for_testing;
}

ScopedMemoryPurgeGateOpenForTesting::~ScopedMemoryPurgeGateOpenForTesting() {
  --g_open_for_testing;
}

}  // namespace nexus
