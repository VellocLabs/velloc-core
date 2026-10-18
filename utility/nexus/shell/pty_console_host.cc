// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/pty_console_host.h"

#include <utility>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
#include "custom_browser/common/custom_browser_switches.h"
#include "custom_browser/common/nexus_runtime_paths.h"

namespace nexus {

namespace {

constexpr char kHostBundled[] = "bundled";
constexpr char kHostSystem[] = "system";

struct ResolvedState {
  base::Lock lock;
  std::optional<PtyConsoleHostTable> table;
  std::optional<PtyConsoleHostTable> test_override;
};

ResolvedState& State() {
  static base::NoDestructor<ResolvedState> state;
  return *state;
}

#if BUILDFLAG(IS_WIN)
// Looks the three entry points up in `module`, trying the `Conpty`-prefixed
// names the console host package exports and then the plain kernel32 names.
// Fills `table` only when all three resolve.
bool ResolveExports(HMODULE module, PtyConsoleHostTable* table) {
  struct Names {
    const char* create;
    const char* resize;
    const char* close;
  };
  static constexpr Names kCandidates[] = {
      {"ConptyCreatePseudoConsole", "ConptyResizePseudoConsole",
       "ConptyClosePseudoConsole"},
      {"CreatePseudoConsole", "ResizePseudoConsole", "ClosePseudoConsole"},
  };
  for (const Names& names : kCandidates) {
    FARPROC create = ::GetProcAddress(module, names.create);
    FARPROC resize = ::GetProcAddress(module, names.resize);
    FARPROC close = ::GetProcAddress(module, names.close);
    if (create && resize && close) {
      table->create =
          reinterpret_cast<PtyConsoleHostTable::CreateFn>(create);
      table->resize =
          reinterpret_cast<PtyConsoleHostTable::ResizeFn>(resize);
      table->close = reinterpret_cast<PtyConsoleHostTable::CloseFn>(close);
      return true;
    }
  }
  return false;
}
#endif  // BUILDFLAG(IS_WIN)

}  // namespace

bool PtyConsoleHostTable::available() const {
#if BUILDFLAG(IS_WIN)
  return create && resize && close && !host.empty();
#else
  return false;
#endif
}

// static
const PtyConsoleHostTable& PtyConsoleHost::Get() {
  ResolvedState& state = State();
  base::AutoLock auto_lock(state.lock);
  if (state.test_override.has_value()) {
    return *state.test_override;
  }
  if (!state.table.has_value()) {
    state.table = Resolve(BundledDllForThisProcess());
  }
  return *state.table;
}

// static
PtyConsoleHostTable PtyConsoleHost::Resolve(const base::FilePath& bundled_dll) {
  PtyConsoleHostTable table;
#if BUILDFLAG(IS_WIN)
  if (!bundled_dll.empty()) {
    HMODULE module = ::LoadLibraryExW(
        bundled_dll.value().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      PLOG(WARNING) << "[NexusShellService] bundled console host failed to "
                       "load, using the system ConPTY: "
                    << bundled_dll.AsUTF8Unsafe();
    } else if (ResolveExports(module, &table)) {
      table.host = kHostBundled;
      return table;
    } else {
      LOG(WARNING) << "[NexusShellService] bundled console host lacks the "
                      "pseudoconsole exports, using the system ConPTY: "
                   << bundled_dll.AsUTF8Unsafe();
      ::FreeLibrary(module);
      table = PtyConsoleHostTable();
    }
  }
  HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
  if (kernel32 && ResolveExports(kernel32, &table)) {
    table.host = kHostSystem;
    return table;
  }
  return PtyConsoleHostTable();
#else
  return table;
#endif
}

// static
base::FilePath PtyConsoleHost::BundledDllForThisProcess(
    const base::FilePath& module_dir) {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line &&
      command_line->GetSwitchValueASCII(
          custom_browser::switches::kConsoleHost) ==
          custom_browser::switches::kConsoleHostSystem) {
    return base::FilePath();
  }
  return module_dir.empty()
             ? custom_browser::GetBundledConsoleHostPath()
             : custom_browser::GetBundledConsoleHostPathIn(module_dir);
}

// static
void PtyConsoleHost::SetTableForTesting(
    std::optional<PtyConsoleHostTable> table) {
  ResolvedState& state = State();
  base::AutoLock auto_lock(state.lock);
  state.test_override = std::move(table);
}

}  // namespace nexus
