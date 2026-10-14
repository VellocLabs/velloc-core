// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SHELL_RESOLVER_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SHELL_RESOLVER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace nexus {

// Description of one external tool that the utility-process service tried
// to locate. Mirrors `nexus.mojom.ShellResolvedTool` but lives in pure C++
// so the resolver can be unit-tested without going through Mojo.
struct ResolvedTool {
  ResolvedTool();
  ~ResolvedTool();
  ResolvedTool(const ResolvedTool&);
  ResolvedTool& operator=(const ResolvedTool&);
  ResolvedTool(ResolvedTool&&);
  ResolvedTool& operator=(ResolvedTool&&);

  std::string id;
  base::FilePath path;
  std::string version;
  bool available = false;
};

// Aggregate result of a host capability probe.
struct ResolvedCapabilities {
  ResolvedCapabilities();
  ~ResolvedCapabilities();
  ResolvedCapabilities(const ResolvedCapabilities&);
  ResolvedCapabilities& operator=(const ResolvedCapabilities&);
  ResolvedCapabilities(ResolvedCapabilities&&);
  ResolvedCapabilities& operator=(ResolvedCapabilities&&);

  // "windows" | "macos" | "linux" | "unknown".
  std::string platform;

  // Default shell selected by `ShellResolver` for the host platform. Null
  // when none of the candidate shells were found.
  std::optional<ResolvedTool> default_shell;

  // All probed tools, in the order returned by `KnownToolIds()`. Tools
  // that were not found still appear here with `available == false` so
  // the LLM can be told *why* a particular request will fail (e.g. "rg
  // not installed").
  std::vector<ResolvedTool> tools;

  // Tool ids the service is willing to Exec/Spawn. Empty in slice 1 (no
  // Exec/Spawn yet); populated in later slices.
  std::vector<std::string> command_allowlist;
};

// Synchronous host-shell probe. Performs PATH lookups and `--version`
// invocations and is therefore blocking — call from a `base::MayBlock()`
// task, never on the Mojo binding thread.
//
// Designed to be called once per utility-process lifetime; the impl in
// `NexusShellServiceImpl` caches the result across all `GetCapabilities`
// calls.
class ShellResolver {
 public:
  // Tools probed by `Resolve()`. Stable, alphabetised, no extensions.
  static std::vector<std::string> KnownToolIds();

  // Performs the full probe and returns the populated capabilities.
  static ResolvedCapabilities Resolve();

  // Locates `tool_id` on PATH (with platform-appropriate extensions on
  // Windows), plus curated host-specific fallbacks such as Git for Windows'
  // bash.exe. Exposed for unit testing.
  static std::optional<base::FilePath> FindOnPath(const std::string& tool_id);

  // Windows-only Git-for-Windows discovery. On non-Windows returns nullopt.
  // Mirrors VS Code-style terminal profile discovery by checking common install
  // locations when bash.exe is not on PATH. Exposed for unit testing.
  static std::optional<base::FilePath> FindGitBashForWindows();

  // Runs `<exe> --version`, captures the first line, trims whitespace,
  // and caps the length. Returns the empty string on any failure (e.g.
  // tool does not implement `--version`). Exposed for unit testing.
  static std::string ProbeVersion(const base::FilePath& exe);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SHELL_RESOLVER_H_
