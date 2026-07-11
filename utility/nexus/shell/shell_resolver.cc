// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
#include "build/build_config.h"

namespace nexus {

namespace {

constexpr size_t kMaxVersionLength = 256;

// Hard cap to keep `--version` from hanging the resolver indefinitely if a
// misbehaving binary refuses to exit. base::GetAppOutput itself does not
// expose a timeout, but utility-process bring-up is fast in practice; if
// this becomes a problem we will switch to base::LaunchProcess + a timed
// WaitForExitWithTimeout in a follow-up slice.
constexpr char kVersionFlag[] = "--version";

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

// Returns the priority-ordered list of candidate ids for the host
// platform's "default shell". Matches the design in
// docs/agents/nexus-process.md: prefer the most modern shell that is
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
  // Order matters only for the resolver's diagnostic output. Keep this
  // alphabetised so the list is easy to scan in tests / logs.
  return {"bash",       "cmd", "git", "powershell", "pwsh",
          "rg",         "sh",  "zsh"};
}

// static
std::optional<base::FilePath> ShellResolver::FindOnPath(
    const std::string& tool_id) {
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

  base::CommandLine cmd(exe);
  cmd.AppendArg(kVersionFlag);

  std::string raw_output;
  if (!base::GetAppOutput(cmd, &raw_output) || raw_output.empty()) {
    return std::string();
  }

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
