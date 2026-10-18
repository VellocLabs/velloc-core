// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_PTY_CONSOLE_HOST_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_PTY_CONSOLE_HOST_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace nexus {

// The pseudoconsole entry points a process uses for its pty children, and
// which console host supplies them (F-31). `host` is "bundled" (the
// conpty.dll shipped next to chrome.dll), "system" (kernel32's ConPTY) or
// empty when neither is usable, in which case `available()` is false.
struct PtyConsoleHostTable {
#if BUILDFLAG(IS_WIN)
  using CreateFn = HRESULT(WINAPI*)(COORD size,
                                    HANDLE input,
                                    HANDLE output,
                                    DWORD flags,
                                    HPCON* out_handle);
  using ResizeFn = HRESULT(WINAPI*)(HPCON handle, COORD size);
  using CloseFn = void(WINAPI*)(HPCON handle);

  CreateFn create = nullptr;
  ResizeFn resize = nullptr;
  CloseFn close = nullptr;
#endif
  std::string host;

  bool available() const;
};

// Resolves the pseudoconsole entry points once per process: the bundled
// console host first, kernel32 second (F-31).
class PtyConsoleHost {
 public:
  // The process-wide table, resolved on the first call and cached. The first
  // call may load a DLL, so make it from a task that may block.
  static const PtyConsoleHostTable& Get();

  // Resolves a table from `bundled_dll` (empty: skip the bundled host) and
  // then kernel32. Pure apart from the library load; what `Get()` runs.
  // Security: `bundled_dll` is loaded by absolute path with the DLL's own
  // directory and System32 as the only search locations.
  static PtyConsoleHostTable Resolve(const base::FilePath& bundled_dll);

  // The bundled DLL `Get()` tries: the one in chrome.dll's directory (or in
  // `module_dir` when given), unless the `--velloc-console-host=system`
  // switch is set or the files are absent.
  static base::FilePath BundledDllForThisProcess(
      const base::FilePath& module_dir = base::FilePath());

  // Replaces (or, with nullopt, restores) what `Get()` returns.
  static void SetTableForTesting(std::optional<PtyConsoleHostTable> table);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_PTY_CONSOLE_HOST_H_
