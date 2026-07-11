// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/safe_command_policy.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "custom_browser/utility/nexus/shell/install_hints.h"

namespace nexus {

namespace {

#if BUILDFLAG(IS_WIN)
constexpr bool kPathIsCaseSensitive = false;
#else
constexpr bool kPathIsCaseSensitive = true;
#endif

bool PathsEqual(const base::FilePath& lhs, const base::FilePath& rhs) {
  if (kPathIsCaseSensitive) {
    return lhs == rhs;
  }
  return base::EqualsCaseInsensitiveASCII(lhs.AsUTF8Unsafe(),
                                          rhs.AsUTF8Unsafe());
}

bool IsForbiddenEnvName(std::string_view name) {
  // Case-insensitive on Windows (env var names are case-insensitive
  // there); case-sensitive on POSIX.
  for (const std::string& forbidden : SafeCommandPolicy::ForbiddenEnvNames()) {
#if BUILDFLAG(IS_WIN)
    if (base::EqualsCaseInsensitiveASCII(name, forbidden)) {
      return true;
    }
#else
    if (name == forbidden) {
      return true;
    }
#endif
  }
  // POSIX-only catch-all: drop anything starting with "DYLD_" or "LD_".
  // These are dropped on Windows too (cheap and harmless).
  if (name.size() >= 5 &&
      (name.substr(0, 5) == "DYLD_" || name.substr(0, 3) == "LD_")) {
    return true;
  }
  return false;
}

#if BUILDFLAG(IS_WIN)
base::EnvironmentMap::key_type ToNativeEnvKey(const std::string& name) {
  return base::UTF8ToWide(name);
}
base::EnvironmentMap::mapped_type ToNativeEnvValue(const std::string& value) {
  return base::UTF8ToWide(value);
}
#else
base::EnvironmentMap::key_type ToNativeEnvKey(const std::string& name) {
  return name;
}
base::EnvironmentMap::mapped_type ToNativeEnvValue(const std::string& value) {
  return value;
}
#endif

}  // namespace

PolicyDecision::PolicyDecision() = default;

PolicyDecision::~PolicyDecision() = default;

PolicyDecision::PolicyDecision(const PolicyDecision&) = default;

PolicyDecision& PolicyDecision::operator=(const PolicyDecision&) = default;

PolicyDecision::PolicyDecision(PolicyDecision&&) = default;

PolicyDecision& PolicyDecision::operator=(PolicyDecision&&) = default;

// static
PolicyDecision SafeCommandPolicy::ResolveCommand(
    const std::string& command,
    const ResolvedCapabilities& caps) {
  PolicyDecision decision;

  if (command.empty()) {
    decision.error_code = "COMMAND_NOT_ALLOWED";
    decision.error_message = "command must be a non-empty tool id or path";
    return decision;
  }

  // Build the fast lookup of allowlisted-and-available ids, plus the
  // companion lookup of "known to the resolver, but not currently
  // available" ids. We need both so we can distinguish:
  //   * COMMAND_NOT_ALLOWED — the id was never in the allowlist (the
  //     model is asking for something the host policy refuses to run).
  //   * BINARY_NOT_FOUND — the id IS allowlisted (i.e. the host wants
  //     to support it), but the resolver couldn't locate the binary on
  //     PATH. This is the actionable case where we surface an
  //     install hint.
  std::vector<const ResolvedTool*> allowed_tools;
  allowed_tools.reserve(caps.tools.size());
  std::vector<const ResolvedTool*> known_but_unavailable;
  known_but_unavailable.reserve(caps.tools.size());
  const bool id_in_allowlist =
      std::ranges::find(caps.command_allowlist, command) !=
      caps.command_allowlist.end();

  for (const ResolvedTool& tool : caps.tools) {
    const bool listed =
        std::ranges::find(caps.command_allowlist, tool.id) !=
        caps.command_allowlist.end();
    if (tool.available && listed) {
      allowed_tools.push_back(&tool);
    } else if (!tool.available && tool.id == command) {
      known_but_unavailable.push_back(&tool);
    }
  }

  base::FilePath candidate = base::FilePath::FromUTF8Unsafe(command);

  if (candidate.IsAbsolute()) {
    for (const ResolvedTool* tool : allowed_tools) {
      if (PathsEqual(tool->path, candidate)) {
        decision.resolved_path = tool->path;
        return decision;
      }
    }
    decision.error_code = "COMMAND_NOT_ALLOWED";
    decision.error_message =
        "absolute path '" + command +
        "' does not match any allowlisted resolved tool path";
    return decision;
  }

  // Treat as a tool id.
  for (const ResolvedTool* tool : allowed_tools) {
    if (tool->id == command) {
      decision.resolved_path = tool->path;
      return decision;
    }
  }

  // The id is allowlisted (or the resolver knows about it) but no
  // available tool matched. This is the missing-binary case — return
  // BINARY_NOT_FOUND so the guest tool layer can surface the install
  // hint to the model, instead of the more opaque COMMAND_NOT_ALLOWED.
  if (!known_but_unavailable.empty() || id_in_allowlist) {
    decision.error_code = "BINARY_NOT_FOUND";
    decision.error_message =
        "tool id '" + command +
        "' is allowlisted on this host but the binary is not available on "
        "PATH; install it and retry";
    decision.install_hint = InstallHintFor(command, caps.platform);
    return decision;
  }

  decision.error_code = "COMMAND_NOT_ALLOWED";
  decision.error_message =
      "tool id '" + command +
      "' is not in the host capability allowlist for the nexus shell "
      "service";
  return decision;
}

// static
bool SafeCommandPolicy::ValidateCwd(const base::FilePath& cwd,
                                    std::string* out_reason) {
  if (cwd.empty()) {
    if (out_reason) {
      *out_reason = "cwd must not be empty";
    }
    return false;
  }
  if (!cwd.IsAbsolute()) {
    if (out_reason) {
      *out_reason = "cwd must be an absolute path";
    }
    return false;
  }
  if (!base::PathExists(cwd)) {
    if (out_reason) {
      *out_reason = "cwd does not exist: " + cwd.AsUTF8Unsafe();
    }
    return false;
  }
  if (!base::DirectoryExists(cwd)) {
    if (out_reason) {
      *out_reason = "cwd is not a directory: " + cwd.AsUTF8Unsafe();
    }
    return false;
  }
  if (out_reason) {
    out_reason->clear();
  }
  return true;
}

// static
base::EnvironmentMap SafeCommandPolicy::BuildSafeEnvironment(
    const std::vector<EnvOverride>& extras) {
  base::EnvironmentMap env;

  std::unique_ptr<base::Environment> parent = base::Environment::Create();
  for (const std::string& name : InheritedEnvNames()) {
    if (IsForbiddenEnvName(name)) {
      // Defence in depth: the inherited list should never overlap the
      // forbidden list, but guard anyway in case someone edits one and
      // not the other.
      continue;
    }
    std::optional<std::string> value = parent->GetVar(name);
    if (!value.has_value() || value->empty()) {
      continue;
    }
    env[ToNativeEnvKey(name)] = ToNativeEnvValue(*value);
  }

  for (const EnvOverride& extra : extras) {
    if (extra.name.empty() || IsForbiddenEnvName(extra.name)) {
      continue;
    }
    env[ToNativeEnvKey(extra.name)] = ToNativeEnvValue(extra.value);
  }

  return env;
}

// static
const std::vector<std::string>& SafeCommandPolicy::ForbiddenEnvNames() {
  // Variables that change the dynamic linker / interpreter behaviour
  // and could be used to coerce the child into running attacker code,
  // or that override module search paths in a way the LLM should never
  // get to control.
  static const std::vector<std::string>* kNames = new std::vector<std::string>{
      "LD_PRELOAD",
      "LD_LIBRARY_PATH",
      "LD_AUDIT",
      "LD_DEBUG",
      "DYLD_INSERT_LIBRARIES",
      "DYLD_LIBRARY_PATH",
      "DYLD_FRAMEWORK_PATH",
      "DYLD_FALLBACK_LIBRARY_PATH",
      "NODE_OPTIONS",
      "PYTHONPATH",
      "PYTHONSTARTUP",
      "PERL5LIB",
      "RUBYOPT",
  };
  return *kNames;
}

// static
const std::vector<std::string>& SafeCommandPolicy::InheritedEnvNames() {
  // Names we *always* want the child to see, copied from the utility
  // process's environment when present. Anything not on this list is
  // dropped — even harmless variables — so the LLM cannot smuggle data
  // by telling the user to "export FOO=secret" in the parent shell.
  static const std::vector<std::string>* kNames = new std::vector<std::string>{
      "PATH",
      "HOME",
      "USER",
      "LOGNAME",
      "LANG",
      "LC_ALL",
      "LC_CTYPE",
      "TZ",
      "TMPDIR",
      // Windows
      "USERPROFILE",
      "SystemRoot",
      "SystemDrive",
      "COMSPEC",
      "windir",
      "TEMP",
      "TMP",
      "ProgramFiles",
      "ProgramFiles(x86)",
      "PROGRAMDATA",
      "APPDATA",
      "LOCALAPPDATA",
  };
  return *kNames;
}

// static
uint32_t SafeCommandPolicy::ClampTimeoutMs(uint32_t requested) {
  if (requested == 0) {
    return SafeCommandLimits::kDefaultTimeoutMs;
  }
  return std::min(requested, SafeCommandLimits::kMaxTimeoutMs);
}

// static
uint32_t SafeCommandPolicy::ClampMaxOutputBytes(uint32_t requested) {
  if (requested == 0) {
    return SafeCommandLimits::kDefaultMaxOutputBytes;
  }
  return std::min(requested, SafeCommandLimits::kMaxMaxOutputBytes);
}

}  // namespace nexus
