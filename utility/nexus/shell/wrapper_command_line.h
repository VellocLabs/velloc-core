// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_WRAPPER_COMMAND_LINE_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_WRAPPER_COMMAND_LINE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace nexus {

// Whether CreateProcess runs `exe` through cmd.exe: a `.cmd` / `.bat` name in
// any case, also with the trailing dots or spaces Windows ignores.
bool IsBatchWrapper(const base::FilePath& exe);

// Why a wrapper launch cannot keep its argv verbatim: `index` is the
// argument (nullopt = the wrapper path itself), `what` names the character
// class. Never carries the value, which may be a credential.
struct WrapperArgProblem {
  std::optional<size_t> index;
  const char* what;
};

// Security: the first problem with launching `wrapper` on `args`. In an
// argument: `& | < >`, which a wrapper that drops the quotes (`%~1`) would
// run as an operator, and `%`, `!`, `^`, `"` or a control character, which
// cmd.exe changes even inside quotes. In the wrapper path (resolved by the
// host, never by a caller): only the second set. Every other character
// reaches the wrapper verbatim on the line BuildWrapperCommandLine builds
// (P-7).
std::optional<WrapperArgProblem> FindWrapperArgProblem(
    const base::FilePath& wrapper,
    const std::vector<std::string>& args);

// Contract: `"<cmd_exe>" /D /V:OFF /S /C ""<wrapper>" <args>"`. /D skips
// AutoRun, /V:OFF keeps delayed expansion off, /S strips exactly the outer
// quotes. The wrapper path is always quoted; an argument is quoted unless it
// is non-empty and only letters, digits and `- _ . / \ : @ +`, so spaces,
// `( ) , ; =` and an empty argument stay literal. Only for arguments
// FindWrapperArgProblem accepted.
std::wstring BuildWrapperCommandLine(const base::FilePath& cmd_exe,
                                     const base::FilePath& wrapper,
                                     const std::vector<std::string>& args);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_WRAPPER_COMMAND_LINE_H_
