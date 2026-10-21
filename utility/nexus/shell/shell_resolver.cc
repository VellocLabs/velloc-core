// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/shell_resolver.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/process/launch.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "custom_browser/common/nexus_runtime_paths.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"

namespace nexus {

namespace {

constexpr size_t kMaxVersionLength = 256;

constexpr char kVersionFlag[] = "--version";

// Hard caps for the `--version` probe. It runs through `RunChildProcess`
// (same no-window launch path as Exec, so the probe never flashes a console
// window on Windows), which enforces the timeout and output cap.
constexpr int kVersionProbeTimeoutMs = 5000;
constexpr uint32_t kVersionProbeMaxOutputBytes = 64 * 1024;

#if BUILDFLAG(IS_WIN)
constexpr char kPathSeparator = ';';
const char* const kExecutableExtensions[] = {".exe", ".com", ".cmd",
                                              ".bat", ".ps1", ""};
#else
constexpr char kPathSeparator = ':';
const char* const kExecutableExtensions[] = {""};
#endif

std::string DetectPlatform() {
#if BUILDFLAG(IS_WIN)
  return "windows";
#elif BUILDFLAG(IS_MAC)
  return "macos";
#elif BUILDFLAG(IS_LINUX)
  return "linux";
#else
  return "unknown";
#endif
}

std::vector<base::FilePath> GetPathDirectories() {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> path_value = env->GetVar("PATH");
  if (!path_value.has_value() || path_value->empty()) {
    return {};
  }

  const std::vector<std::string_view> parts = base::SplitStringPiece(
      *path_value, std::string(1, kPathSeparator), base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);

  std::vector<base::FilePath> dirs;
  dirs.reserve(parts.size());
  for (std::string_view part : parts) {
    dirs.push_back(base::FilePath::FromUTF8Unsafe(part));
  }
  return dirs;
}

// Returns true if `candidate` exists and is a regular file (or, on POSIX,
// at least not a directory). PATH lookup intentionally does not check the
// executable bit because that is OS-specific and the subsequent --version
// probe will surface non-executable files anyway.
bool IsCandidateUsable(const base::FilePath& candidate) {
  if (!base::PathExists(candidate)) {
    return false;
  }
  return !base::DirectoryExists(candidate);
}

#if BUILDFLAG(IS_WIN)
std::optional<base::FilePath> EnvPath(const char* name) {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> value = env->GetVar(std::string(name));
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return base::FilePath::FromUTF8Unsafe(*value);
}

void AppendGitShellCandidatesFromRoot(const base::FilePath& root,
                                      const char* exe_name,
                                      std::vector<base::FilePath>* out) {
  if (root.empty()) {
    return;
  }
  out->push_back(root.AppendASCII("bin").AppendASCII(exe_name));
  out->push_back(root.AppendASCII("usr").AppendASCII("bin").AppendASCII(exe_name));
}

// Candidate paths for a Git for Windows shell (`exe_name` is bash.exe or
// sh.exe). The *_BASH_PATH / SHELL overrides name a bash, so they only apply
// when `include_bash_overrides` is set.
std::vector<base::FilePath> GitShellCandidatePathsForWindows(
    const char* exe_name,
    bool include_bash_overrides) {
  std::vector<base::FilePath> candidates;

  if (include_bash_overrides) {
    for (const char* env_name :
         {"NEXUS_BASH_PATH", "VELLOC_BASH_PATH", "SHELL"}) {
      if (std::optional<base::FilePath> override_path = EnvPath(env_name);
          override_path.has_value() && !override_path->empty()) {
        candidates.push_back(*override_path);
      }
    }
  }

  if (std::optional<base::FilePath> program_files = EnvPath("ProgramFiles")) {
    AppendGitShellCandidatesFromRoot(program_files->AppendASCII("Git"),
                                     exe_name, &candidates);
  }
  if (std::optional<base::FilePath> program_files_x86 = EnvPath("ProgramFiles(x86)")) {
    AppendGitShellCandidatesFromRoot(program_files_x86->AppendASCII("Git"),
                                     exe_name, &candidates);
  }
  if (std::optional<base::FilePath> local_app_data = EnvPath("LOCALAPPDATA")) {
    AppendGitShellCandidatesFromRoot(
        local_app_data->AppendASCII("Programs").AppendASCII("Git"), exe_name,
        &candidates);
  }

  // Literal fallbacks for stripped environments where ProgramFiles variables are
  // missing but Git is installed in the default location.
  AppendGitShellCandidatesFromRoot(
      base::FilePath(FILE_PATH_LITERAL("C:\\Program Files\\Git")), exe_name,
      &candidates);
  AppendGitShellCandidatesFromRoot(
      base::FilePath(FILE_PATH_LITERAL("C:\\Program Files (x86)\\Git")),
      exe_name, &candidates);

  std::vector<base::FilePath> unique;
  for (const base::FilePath& candidate : candidates) {
    const std::string normalized = base::ToLowerASCII(candidate.AsUTF8Unsafe());
    const bool seen = std::ranges::any_of(unique, [&](const base::FilePath& p) {
      return base::ToLowerASCII(p.AsUTF8Unsafe()) == normalized;
    });
    if (!seen) {
      unique.push_back(candidate);
    }
  }
  return unique;
}

std::optional<base::FilePath> FindGitShellForWindows(
    const char* exe_name,
    bool include_bash_overrides) {
  for (const base::FilePath& candidate :
       GitShellCandidatePathsForWindows(exe_name, include_bash_overrides)) {
    if (IsCandidateUsable(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}
#endif  // BUILDFLAG(IS_WIN)

// Returns the priority-ordered list of candidate ids for the host
// platform's "default shell". Matches the design in
// P-7: prefer the most modern shell that is
// actually installed.
std::vector<std::string> DefaultShellPreference() {
#if BUILDFLAG(IS_WIN)
  return {"pwsh", "powershell", "cmd"};
#elif BUILDFLAG(IS_MAC)
  return {"zsh", "bash", "sh"};
#else
  return {"bash", "zsh", "sh"};
#endif
}

}  // namespace

ResolvedTool::ResolvedTool() = default;

ResolvedTool::~ResolvedTool() = default;

ResolvedTool::ResolvedTool(const ResolvedTool&) = default;

ResolvedTool& ResolvedTool::operator=(const ResolvedTool&) = default;

ResolvedTool::ResolvedTool(ResolvedTool&&) = default;

ResolvedTool& ResolvedTool::operator=(ResolvedTool&&) = default;

ResolvedCapabilities::ResolvedCapabilities() = default;

ResolvedCapabilities::~ResolvedCapabilities() = default;

ResolvedCapabilities::ResolvedCapabilities(const ResolvedCapabilities&) =
    default;

ResolvedCapabilities& ResolvedCapabilities::operator=(
    const ResolvedCapabilities&) = default;

ResolvedCapabilities::ResolvedCapabilities(ResolvedCapabilities&&) = default;

ResolvedCapabilities& ResolvedCapabilities::operator=(
    ResolvedCapabilities&&) = default;

// static
std::vector<std::string> ShellResolver::KnownToolIds() {
  // Kept alphabetised (a unit test enforces it): shells + dev tools for the
  // Bash/Grep tools, and script-runtime launchers for user-configured stdio MCP
  // servers — no wider reach than the Bash tool already has.
  return {"bash",   "bun",  "bunx",       "cmd",  "deno",
          "docker", "git",  "node",       "npm",  "npx",
          "pnpm",   "powershell", "pwsh", "python", "python3",
          "rg",     "sh",   "uv",         "uvx",  "zsh"};
}

// static
std::optional<base::FilePath> ShellResolver::FindGitBashForWindows() {
#if BUILDFLAG(IS_WIN)
  return FindGitShellForWindows("bash.exe", /*include_bash_overrides=*/true);
#else
  return std::nullopt;
#endif
}

// static
std::optional<base::FilePath> ShellResolver::FindOnPath(
    const std::string& tool_id) {
  // Bundled binaries first: the Windows installer ships a signed rg.exe in the
  // version folder (next to chrome.dll), and that pinned copy wins over whatever rg the host PATH
  // carries — Grep's flags are written against that version, and a packaged
  // build never needs the ~/.velloc/bin download below.
  base::FilePath bundled = custom_browser::GetBundledBinaryPath(tool_id);
  if (!bundled.empty() && IsCandidateUsable(bundled)) {
    return bundled;
  }

  // Special-case `cmd` on Windows: it lives in %SystemRoot%\System32 and
  // PATH lookup will normally find it, but System32 is sometimes pruned
  // from sandboxed environments. Fall through to the PATH walk first and
  // then check the absolute fallback.
  for (const base::FilePath& dir : GetPathDirectories()) {
    for (const char* ext : kExecutableExtensions) {
      base::FilePath candidate =
          dir.AppendASCII(tool_id + std::string(ext));
      if (IsCandidateUsable(candidate)) {
        return candidate;
      }
    }
  }

#if BUILDFLAG(IS_WIN)
  if (tool_id == "bash") {
    std::optional<base::FilePath> git_bash = FindGitBashForWindows();
    if (git_bash.has_value()) {
      return git_bash;
    }
  }

  // Git for Windows ships sh.exe beside bash.exe, but its installer only puts
  // Git\cmd on PATH; stdio MCP configs written as `/bin/sh` need it found.
  if (tool_id == "sh") {
    std::optional<base::FilePath> git_sh =
        FindGitShellForWindows("sh.exe", /*include_bash_overrides=*/false);
    if (git_sh.has_value()) {
      return git_sh;
    }
  }

  if (tool_id == "cmd") {
    std::unique_ptr<base::Environment> env = base::Environment::Create();
    std::optional<std::string> sysroot = env->GetVar("SystemRoot");
    if (sysroot.has_value() && !sysroot->empty()) {
      base::FilePath candidate =
          base::FilePath::FromUTF8Unsafe(*sysroot)
              .AppendASCII("System32")
              .AppendASCII("cmd.exe");
      if (IsCandidateUsable(candidate)) {
        return candidate;
      }
    }
  }
#endif

  // Nexus managed binaries: a tool the installer downloaded into
  // ~/.velloc/bin/<id>/<id>[.exe] resolves even when it isn't on the host
  // PATH, so a bare `Bash: rg ...` finds it too. NOTE: the shell service
  // caches capabilities for its lifetime, so a binary downloaded AFTER
  // the first resolve is only seen here on a later resolve. The Grep path
  // does not depend on this — it execs the absolute managed path, which
  // SafeCommandPolicy allows directly (see its absolute-path branch).
  base::FilePath managed = custom_browser::GetManagedBinaryPath(tool_id);
  if (!managed.empty() && IsCandidateUsable(managed)) {
    return managed;
  }

  return std::nullopt;
}

// static
std::string ShellResolver::ProbeVersion(const base::FilePath& exe) {
  // `cmd.exe` does not implement --version (it would interpret it as a
  // command and fail), and the legacy Windows PowerShell host prints its
  // version to stderr only. Skip the probe for those and let the impl
  // report `available = true` with an empty version.
#if BUILDFLAG(IS_WIN)
  const std::string base_name = exe.BaseName().AsUTF8Unsafe();
  if (base::EqualsCaseInsensitiveASCII(base_name, "cmd.exe") ||
      base::EqualsCaseInsensitiveASCII(base_name, "powershell.exe")) {
    return std::string();
  }
#endif

  // Run the probe through the shared child-process runner instead of
  // base::GetAppOutput so it launches with CREATE_NO_WINDOW on Windows (no
  // console window flashing on screen) and honours a timeout. The probe needs
  // the host PATH / DLL search paths, so it inherits the parent environment.
  ChildProcessRequest req;
  req.exe = exe;
  req.args = {kVersionFlag};
  req.inherit_environment = true;
  req.timeout = base::Milliseconds(kVersionProbeTimeoutMs);
  req.max_output_bytes = kVersionProbeMaxOutputBytes;

  ChildProcessOutcome outcome = RunChildProcess(req);
  if (!outcome.launched || outcome.timed_out || outcome.stdout_bytes.empty()) {
    return std::string();
  }

  std::string raw_output(outcome.stdout_bytes.begin(),
                         outcome.stdout_bytes.end());
  std::string_view first_line(raw_output);
  const size_t newline = first_line.find_first_of("\r\n");
  if (newline != std::string_view::npos) {
    first_line = first_line.substr(0, newline);
  }
  std::string trimmed(base::TrimWhitespaceASCII(first_line, base::TRIM_ALL));
  if (trimmed.size() > kMaxVersionLength) {
    trimmed.resize(kMaxVersionLength);
  }
  return trimmed;
}

// static
ResolvedCapabilities ShellResolver::Resolve() {
  ResolvedCapabilities caps;
  caps.platform = DetectPlatform();

  for (const std::string& tool_id : KnownToolIds()) {
    ResolvedTool tool;
    tool.id = tool_id;
    std::optional<base::FilePath> path = FindOnPath(tool_id);
    if (path.has_value()) {
      tool.path = *path;
      tool.version = ProbeVersion(*path);
      tool.available = true;
    }
    caps.tools.push_back(std::move(tool));
  }

  // Pick the highest-priority shell that actually resolved.
  for (const std::string& candidate_id : DefaultShellPreference()) {
    const auto it = std::ranges::find_if(
        caps.tools,
        [&](const ResolvedTool& t) { return t.id == candidate_id; });
    if (it != caps.tools.end() && it->available) {
      caps.default_shell = *it;
      break;
    }
  }

  // Slice 2 allowlist: every known tool that actually resolved on the
  // host is exposed. The allowlist is intentionally a subset of
  // KnownToolIds() — adding a binary to PATH is not enough to make it
  // executable through the bridge; it must also have a slot in
  // KnownToolIds() (and therefore a hardcoded entry in this file).
  // Future slices may further intersect this with a profile policy
  // (e.g. enterprise admin disables `cmd` even when present).
  for (const ResolvedTool& tool : caps.tools) {
    if (tool.available) {
      caps.command_allowlist.push_back(tool.id);
    }
  }

  return caps;
}

}  // namespace nexus
