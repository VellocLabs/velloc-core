#include "custom_browser/browser/nexus/tools/process_tool/process_tool.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/tools/nexus_ripgrep_runner.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

// =============================================================================
// File-local helpers — kept anonymous so they don't pollute the
// nexus:: namespace and so we can iterate without touching the header.
// =============================================================================
namespace {

constexpr int kDefaultBashTimeoutMs = 120000;
constexpr int kMaxBashTimeoutMs = 600000;
constexpr int kMaxOutputBytes = 30000;
constexpr char kProcessUnavailableCode[] = "process_unavailable";
// `run_in_background` is served by the runtime, which starts the command as a
// persistent host child (`shell.spawn` + `persist`) and owns the task. The host
// keeps no background-shell registry any more (removed under interface 1),
// so an explicit background request that still reaches the hosted tool is
// refused loudly instead of silently blocking in the foreground.
constexpr char kBackgroundUnsupportedCode[] = "background_unsupported";

// Per-platform shell invocation. Mirrors `buildShellInvocation` in the
// retired BashTool.ts but trimmed to one shell per platform — the
// utility-process SafeCommandPolicy is the source of truth for what
// binaries are allowed, so picking a sensible default per OS is enough.
struct ShellInvocation {
  std::string command;            // Tool id, e.g. "bash" or "cmd".
  std::vector<std::string> args;  // argv excluding argv[0].
};

ShellInvocation BuildShellInvocation(const std::string& command_line) {
  ShellInvocation inv;
#if BUILDFLAG(IS_WIN)
  // Windows: run git-bash directly. The runtime offers the Bash tool on
  // Windows only when `exec.listShells` reports `bash` (see
  // api/shell/host_shell.h); on a no-bash host the model gets the
  // PowerShell tool instead. We invoke bash explicitly (RunBashOnUI sets
  // use_shell=false) rather than wrapping in cmd/pwsh, so bash syntax works.
  inv.command = "bash";
  inv.args = {"-c", command_line};
#else
  // Default to /bin/sh; the utility process resolves it against the
  // SafeCommandPolicy allowlist. If a host has bash but not sh the
  // policy already accepts both.
  inv.command = "sh";
  inv.args = {"-c", command_line};
#endif
  return inv;
}

// Hook-exec variant: run `command` in the explicitly requested shell
// rather than the platform default. bash/sh/zsh use `-c <command>`;
// pwsh/powershell use the non-interactive form
// `-NoProfile -NonInteractive -Command`. The shell id is resolved by the
// utility process against the SafeCommandPolicy allowlist; an unavailable
// shell surfaces as BINARY_NOT_FOUND (the JS runner falls back).
ShellInvocation BuildHookShellInvocation(const std::string& shell_id,
                                         const std::string& command) {
  ShellInvocation inv;
  inv.command = shell_id;
  if (shell_id == "pwsh" || shell_id == "powershell") {
    inv.args = {"-NoProfile", "-NonInteractive", "-Command", command};
  } else {
    inv.args = {"-c", command};
  }
  return inv;
}

// Security: `hook_shell` is honoured only for these shell ids (keep in sync
// with the guest's `resolveShellPreference`); any other value runs through the
// normal Bash path instead of being launched as a command.
bool IsKnownHookShellId(const std::string& shell_id) {
  return shell_id == "bash" || shell_id == "sh" || shell_id == "zsh" ||
         shell_id == "pwsh" || shell_id == "powershell";
}

// Parse the hook_env JSON object string into mojo ShellEnvEntry pairs.
// Invalid / non-object JSON yields an empty list (the hook still runs,
// just without the extra env). Non-string values are skipped.
std::vector<mojom::ShellEnvEntryPtr> ParseHookEnv(const std::string& json) {
  std::vector<mojom::ShellEnvEntryPtr> out;
  if (json.empty()) {
    return out;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return out;
  }
  for (const auto [key, value] : parsed->GetDict()) {
    if (value.is_string()) {
      auto entry = mojom::ShellEnvEntry::New();
      entry->name = key;
      entry->value = value.GetString();
      out.push_back(std::move(entry));
    }
  }
  return out;
}

// Ensures `text` is valid UTF-8 (invalid sequences → U+FFFD): base::Value
// DCHECKs UTF-8, and console code pages (cp1252, cp936) are not.
std::string EnsureUtf8(std::string text) {
  if (base::IsStringUTF8AllowingNoncharacters(text)) {
    return text;
  }
  return base::UTF16ToUTF8(base::UTF8ToUTF16(text));
}

// Decode a base::BigBuffer into a UTF-8 std::string capped at
// `kMaxOutputBytes`. Invalid UTF-8 in the captured bytes is normalised
// to U+FFFD via `EnsureUtf8` — see that helper for why.
std::string BigBufferToString(const mojo_base::BigBuffer& buffer, size_t cap) {
  base::span<const uint8_t> bytes(buffer);
  size_t n = std::min(bytes.size(), cap);
  if (n == 0) {
    return std::string();
  }
  return EnsureUtf8(
      std::string(reinterpret_cast<const char*>(bytes.data()), n));
}

// Extract the leading "base command" from a command line for the
// command-semantic table. Mirrors the TS `extractBaseCommand`:
//   1. Split on `||`, `&&`, `;`, `|`; take the LAST segment (it
//      determines the exit code in POSIX shells).
//   2. Strip leading env-var assignments (FOO=bar BAR=baz cmd).
//   3. Take the first whitespace-separated token.
//   4. Strip surrounding quotes.
std::string ExtractBaseCommand(const std::string& command) {
  // Crude split: find the last index of any of the boundary chars and
  // take the substring after it. We don't care about correctness for
  // pathological commands; the goal is to pick `grep` out of
  // `cat foo | grep bar`.
  size_t boundary = std::string::npos;
  for (const char* sep : {"||", "&&", ";", "|"}) {
    size_t p = command.rfind(sep);
    if (p != std::string::npos &&
        (boundary == std::string::npos || p > boundary)) {
      boundary = p + std::string(sep).size();
    }
  }
  std::string segment =
      (boundary == std::string::npos) ? command : command.substr(boundary);
  // Trim leading whitespace.
  size_t start = segment.find_first_not_of(" \t");
  if (start == std::string::npos) {
    return "";
  }
  segment = segment.substr(start);
  // Strip leading env-var assignments: `[A-Z_][A-Z0-9_]*=...<space>`.
  while (!segment.empty()) {
    size_t eq = segment.find('=');
    if (eq == std::string::npos) {
      break;
    }
    bool looks_like_env = true;
    if (eq == 0) {
      looks_like_env = false;
    }
    for (size_t i = 0; i < eq && looks_like_env; ++i) {
      char c = segment[i];
      if (!(base::IsAsciiUpper(c) || base::IsAsciiDigit(c) || c == '_')) {
        looks_like_env = false;
      }
    }
    if (!looks_like_env) {
      break;
    }
    // Find the trailing whitespace after the assignment value.
    size_t ws = segment.find_first_of(" \t", eq);
    if (ws == std::string::npos) {
      // Whole segment is just an assignment — treat as the command.
      break;
    }
    segment = segment.substr(ws);
    size_t s = segment.find_first_not_of(" \t");
    if (s == std::string::npos) {
      break;
    }
    segment = segment.substr(s);
  }
  // First whitespace-separated token.
  size_t end = segment.find_first_of(" \t");
  std::string first =
      (end == std::string::npos) ? segment : segment.substr(0, end);
  // Strip surrounding quotes.
  if (first.size() >= 2 && (first.front() == '\'' || first.front() == '"') &&
      first.front() == first.back()) {
    first = first.substr(1, first.size() - 2);
  }
  return first;
}

struct CommandInterpretation {
  bool is_error = true;
  std::string message;
};

// Mirrors the TS `commandSemantics` table. Exit-1 from these tools
// means "no matches" / "false" rather than failure; everything else
// gets the default "exit code N" interpretation.
CommandInterpretation InterpretExit(const std::string& base, int exit) {
  if (base == "grep" || base == "rg") {
    return {exit >= 2, exit == 1 ? "no_matches" : ""};
  }
  if (base == "find") {
    return {exit >= 2, exit == 1 ? "inaccessible_dirs" : ""};
  }
  if (base == "diff") {
    return {exit >= 2, exit == 1 ? "files_differ" : ""};
  }
  if (base == "test" || base == "[") {
    return {exit >= 2, exit == 1 ? "condition_false" : ""};
  }
  // Default semantic: the runtime renders "exit code N" from exit_code.
  return {exit != 0, exit != 0 ? "failed" : ""};
}

// =============================================================================
// Grep helpers — port of TS GrepTool's argv-shaping + result-shaping
// =============================================================================

constexpr int kDefaultGrepHeadLimit = 250;
constexpr int kGrepMaxColumns = 500;
// Context lines defaulted onto a `content` grep whose `path` is one regular
// file and that carries no -A/-B/-C/context of its own. See
// ApplySingleFileGrepDefaults for why 5 and not more.
constexpr int kSingleFileGrepDefaultContext = 5;

// VCS dirs ripgrep should never recurse into. ripgrep already
// gitignore-respects but we belt-and-suspender against `.git` etc.
const char* const kGrepExcludedDirs[] = {".git", ".svn", ".hg",
                                         ".bzr", ".jj",  ".sl"};

// Resolve `path` against `cwd_or_default`: absolute paths used as-is,
// relative paths joined onto the default. Empty `path` returns the
// default. Returns base::FilePath() when both are empty (caller
// surfaces a clean error).
base::FilePath ResolveAgainstCwd(const std::string& path,
                                 const base::FilePath& default_root) {
  if (path.empty()) {
    return default_root;
  }
  base::FilePath candidate = base::FilePath::FromUTF8Unsafe(path);
  if (candidate.IsAbsolute()) {
    return candidate;
  }
  if (default_root.empty()) {
    return base::FilePath();
  }
  return default_root.Append(candidate);
}

// Split `glob` on whitespace; if a token contains brace-expansion
// (`{a,b}`), emit it intact for ripgrep to expand; otherwise comma-
// separated tokens become separate `--glob` patterns. Mirrors the TS
// `splitGlobPatterns`.
std::vector<std::string> SplitGlobPatterns(const std::string& glob) {
  std::vector<std::string> out;
  for (const std::string& part : base::SplitString(
           glob, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (part.find('{') != std::string::npos &&
        part.find('}') != std::string::npos) {
      out.push_back(part);
      continue;
    }
    for (const std::string& inner : base::SplitString(
             part, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      out.push_back(inner);
    }
  }
  return out;
}

// Build the rg argv for a Grep call. Mirrors `buildRipgrepArgs` from
// the retired GrepTool.ts. `pattern` is the user's regex.
std::vector<std::string> BuildRipgrepArgs(
    const ::nexus::generated::FilesGrepInput& input,
    const std::string& output_mode,
    bool show_line_numbers) {
  std::vector<std::string> args;
  args.push_back("--hidden");
  for (const char* dir : kGrepExcludedDirs) {
    args.push_back("--glob");
    args.push_back(base::StrCat({"!", dir}));
  }
  args.push_back("--max-columns");
  args.push_back(base::NumberToString(kGrepMaxColumns));

  if (input.multiline.value_or(false)) {
    args.push_back("-U");
    args.push_back("--multiline-dotall");
  }
  if (input.case_insensitive.value_or(false)) {
    args.push_back("-i");
  }

  if (output_mode == "files_with_matches") {
    args.push_back("-l");
  } else if (output_mode == "count") {
    args.push_back("-c");
  }

  if (show_line_numbers && output_mode == "content") {
    args.push_back("-n");
  }

  if (output_mode == "content") {
    if (input.context.has_value()) {
      args.push_back("-C");
      args.push_back(base::NumberToString(*input.context));
    } else {
      if (input.context_c.has_value()) {
        args.push_back("-C");
        args.push_back(base::NumberToString(*input.context_c));
      } else {
        if (input.context_before.has_value()) {
          args.push_back("-B");
          args.push_back(base::NumberToString(*input.context_before));
        }
        if (input.context_after.has_value()) {
          args.push_back("-A");
          args.push_back(base::NumberToString(*input.context_after));
        }
      }
    }
  }

  // Patterns starting with `-` need `-e` so rg doesn't read them as
  // flags.
  if (!input.pattern.empty() && input.pattern[0] == '-') {
    args.push_back("-e");
    args.push_back(input.pattern);
  } else {
    args.push_back(input.pattern);
  }

  if (input.type.has_value() && !input.type->empty()) {
    args.push_back("--type");
    args.push_back(*input.type);
  }

  if (input.glob.has_value() && !input.glob->empty()) {
    for (const std::string& pat : SplitGlobPatterns(*input.glob)) {
      args.push_back("--glob");
      args.push_back(pat);
    }
  }

  return args;
}

// A Grep on one regular file defaults to content mode with -C 5. Explicit
// output_mode or context always wins; directories and UNC targets are left
// alone.
void ApplySingleFileGrepDefaults(::nexus::generated::FilesGrepInput& input,
                                 std::string& output_mode,
                                 bool target_is_file) {
  if (!target_is_file) {
    return;
  }
  if (!input.output_mode.has_value()) {
    output_mode = "content";
  }
  if (output_mode != "content") {
    return;
  }
  if (input.context.has_value() || input.context_c.has_value() ||
      input.context_before.has_value() || input.context_after.has_value()) {
    return;
  }
  input.context_c = kSingleFileGrepDefaultContext;
}

// Apply head_limit + offset slicing matching the TS `applyHeadLimit`.
// Returns (sliced, applied_limit_when_truncated, true_if_truncated).
struct SliceResult {
  std::vector<std::string> items;
  int applied_limit = 0;  // 0 means "not truncated / no limit reported"
  bool truncated = false;
};

SliceResult ApplyHeadLimit(std::vector<std::string>&& items,
                           int head_limit,
                           int offset) {
  SliceResult out;
  size_t off = (offset > 0) ? static_cast<size_t>(offset) : 0;
  if (off >= items.size()) {
    return out;
  }
  if (head_limit == 0) {
    out.items.assign(std::make_move_iterator(items.begin() + off),
                     std::make_move_iterator(items.end()));
    return out;
  }
  size_t cap = static_cast<size_t>(head_limit);
  size_t end = std::min(off + cap, items.size());
  out.items.assign(std::make_move_iterator(items.begin() + off),
                   std::make_move_iterator(items.begin() + end));
  if (items.size() - off > cap) {
    out.truncated = true;
    out.applied_limit = head_limit;
  }
  return out;
}

// NOTE: ripgrep stdout → lines parsing now lives in NexusRipgrepRunner
// (tools/nexus_ripgrep_runner.cc), shared by Grep and Glob. The runner hands
// back already-parsed `lines`, so the Grep path below only shapes them.

// Convert an absolute path to a path relative to `cwd`: if the relative form would escape cwd (starts with ".."),
// keep the absolute path. Saves tokens in tool output.
std::string ToRelativePath(const base::FilePath& abs,
                           const base::FilePath& cwd) {
  if (cwd.empty() || abs.empty()) {
    return abs.AsUTF8Unsafe();
  }
  base::FilePath rel;
  if (!cwd.AppendRelativePath(abs, &rel)) {
    return abs.AsUTF8Unsafe();
  }
  std::string rel_str = rel.AsUTF8Unsafe();
  if (base::StartsWith(rel_str, "..", base::CompareCase::SENSITIVE)) {
    return abs.AsUTF8Unsafe();
  }
  return rel_str;
}

// Relativize the file-path prefix of a ripgrep output line. Content lines
// look like "/abs/path:linenum:content"; count lines "/abs/path:count".
// `sep` is the separator strategy: first colon (content) or last colon
// (count). Returns the line with the path portion made relative to `cwd`.
std::string RelativizeGrepLine(const std::string& line,
                               const base::FilePath& cwd,
                               bool use_last_colon) {
  size_t colon = use_last_colon ? line.find_last_of(':')
                                : line.find(':');
  if (colon == std::string::npos || colon == 0) {
    return line;
  }
  std::string path_part = line.substr(0, colon);
  std::string rest = line.substr(colon);
  base::FilePath abs_path = base::FilePath::FromUTF8Unsafe(path_part);
  if (!abs_path.IsAbsolute()) {
    return line;
  }
  return ToRelativePath(abs_path, cwd) + rest;
}

// Sort file paths by modification time descending (most recent first),
// the files_with_matches order (spec 2-4). Falls back to filename
// ascending as a tiebreaker. Paths that can't be stat'd sort as mtime 0.
void SortByMtimeDesc(std::vector<std::string>* paths) {
  struct Entry {
    std::string path;
    int64_t mtime_ms;
  };
  std::vector<Entry> entries;
  entries.reserve(paths->size());
  for (auto& p : *paths) {
    base::File::Info info;
    int64_t mtime = 0;
    if (base::GetFileInfo(base::FilePath::FromUTF8Unsafe(p), &info)) {
      mtime = info.last_modified.InMillisecondsSinceUnixEpoch();
    }
    entries.push_back({std::move(p), mtime});
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.mtime_ms != b.mtime_ms) {
      return a.mtime_ms > b.mtime_ms;  // descending
    }
    return a.path < b.path;  // filename tiebreaker ascending
  });
  paths->clear();
  paths->reserve(entries.size());
  for (auto& e : entries) {
    paths->push_back(std::move(e.path));
  }
}

}  // namespace

// =============================================================================
// Construction / lifecycle
// =============================================================================

ProcessTool::ProcessTool(ToolSession* session,
                         Profile* profile,
                         const std::string& session_key)
    : ToolBase(session, profile, session_key),
      // Shared per-Profile launcher (NOT owned) so the background shells this
      // tool spawns live in the same utility process + registry the guest
      // `shell.*` capability observes/kills. See GetSharedShellLauncher.
      launcher_(profile ? &GetSharedShellLauncher(profile) : nullptr) {}

ProcessTool::~ProcessTool() {
  // Defensive — invalidate WeakPtrs so any pending mojo callback to THIS tool
  // drops cleanly. The launcher is shared (Profile-owned) — NOT shut down here.
  OnStop(OnStopReason::kDestructor);
}

void ProcessTool::OnStop(OnStopReason reason) {
  LOG(INFO) << "[ProcessTool] OnStop session=" << GetSessionKey()
            << " reason=" << OnStopReasonToString(reason);
  // Teardown reasons only: invalidating on per-turn stops would break later Bash
  // calls in the warm session.
  const bool teardown = reason == OnStopReason::kSessionDeleted ||
                        reason == OnStopReason::kDisconnected ||
                        reason == OnStopReason::kShutdown ||
                        reason == OnStopReason::kDestructor;
  if (teardown) {
    weak_ptr_factory_.InvalidateWeakPtrs();
    // Drop any in-flight rg exec / auto-install callback.
    ripgrep_runner_.Reset();
    // NOTE: do NOT shut down `launcher_` — it is the per-Profile SHARED
    // launcher (other sessions of this profile + the guest `shell.*`
    // capability use the same utility process). It self-recycles on idle.
  }
}

// =============================================================================
// RunBashOnUI override
// =============================================================================

void ProcessTool::RunBashOnUI(::nexus::generated::ExecBashInput input,
                              ToolCallback done) {
  // The dispatcher already validated input shape (command is required
  // string). We add semantic validation here.
  if (input.command.empty()) {
    base::DictValue params;
    params.Set("reason", "command_empty");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }
  if (input.run_in_background.value_or(false)) {
    std::move(done).Reject(kBackgroundUnsupportedCode, base::DictValue());
    return;
  }

  ToolSession* session = GetSession();
  // Per-call cwd override: a worktree-isolated subagent sets `cwd` so its
  // commands run in its own checkout instead of the one shared session cwd
  // (parallel-safe — no race on a single session value). Absolute only,
  // mirroring ToolSession::set_cwd. Absent → fall back to the session cwd
  // (the default path for the main agent).
  base::FilePath cwd;
  if (input.cwd.has_value() && !input.cwd->empty()) {
    cwd = base::FilePath::FromUTF8Unsafe(*input.cwd);
    if (!cwd.IsAbsolute()) {
      base::DictValue params;
      params.Set("reason", "cwd_not_absolute");
      std::move(done).Reject("invalid_args", std::move(params));
      return;
    }
  } else {
    cwd = session ? session->cwd() : base::FilePath();
  }
  if (cwd.empty()) {
    base::DictValue params;
    params.Set("reason", "no_cwd");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }
  if (!launcher_) {
    std::move(done).Reject(kProcessUnavailableCode, base::DictValue());
    return;
  }

  int timeout_ms = input.timeout.value_or(kDefaultBashTimeoutMs);
  if (timeout_ms <= 0) {
    timeout_ms = kDefaultBashTimeoutMs;
  } else if (timeout_ms > kMaxBashTimeoutMs) {
    timeout_ms = kMaxBashTimeoutMs;
  }

  // Hook-exec path: the internal `hook_shell` field runs that shell directly
  // with hook input on stdin and returns the raw exit code + output for the
  // guest's hook protocol. Only known shell ids take it (IsKnownHookShellId).
  if (input.hook_shell.has_value() && IsKnownHookShellId(*input.hook_shell)) {
    ShellInvocation inv =
        BuildHookShellInvocation(*input.hook_shell, input.command);

    auto hook_options = mojom::ShellSpawnOptions::New();
    hook_options->command = inv.command;
    hook_options->args = std::move(inv.args);
    hook_options->cwd = cwd;
    hook_options->use_shell = false;
    hook_options->env = ParseHookEnv(input.hook_env.value_or(std::string()));
    hook_options->stdin_data = input.hook_stdin.value_or(std::string());
    hook_options->timeout_ms = static_cast<uint32_t>(timeout_ms);
    hook_options->max_output_bytes = static_cast<uint32_t>(kMaxOutputBytes);

    mojo::Remote<mojom::NexusShellService>& hook_remote =
        launcher_->GetService();
    hook_remote->Exec(
        std::move(hook_options),
        base::BindOnce(&ProcessTool::OnHookExecDone,
                       weak_ptr_factory_.GetWeakPtr(), std::move(done)));
    return;
  }

  ShellInvocation invocation = BuildShellInvocation(input.command);

  auto options = mojom::ShellSpawnOptions::New();
  options->command = invocation.command;
  options->args = std::move(invocation.args);
  options->cwd = cwd;
#if BUILDFLAG(IS_WIN)
  // git-bash is invoked directly (command="bash"); no default-shell wrapper.
  options->use_shell = false;
#else
  // POSIX: run through the resolved default shell (unchanged behaviour).
  options->use_shell = true;
#endif
  options->timeout_ms = static_cast<uint32_t>(timeout_ms);
  options->max_output_bytes = static_cast<uint32_t>(kMaxOutputBytes);
  // Foreground Bash uses Spawn + ReadBackgroundOutput polling so it can emit
  // live progress; `timeout_ms` is the utility's hard kill. (The old
  // auto-background-on-timeout handoff is gone with the host registry: the
  // runtime backgrounds a long command itself before it reaches the host.)

  mojo::Remote<mojom::NexusShellService>& remote = launcher_->GetService();

  remote->Spawn(std::move(options),
                base::BindOnce(&ProcessTool::OnForegroundSpawnDone,
                               weak_ptr_factory_.GetWeakPtr(), std::move(done),
                               input.command, base::TimeTicks::Now(),
                               std::string("bash")));
}


void ProcessTool::OnForegroundSpawnDone(
    ToolCallback done,
    std::string original_command,
    base::TimeTicks start_time,
    std::string tool_label,
    bool ok,
    const std::optional<std::string>& shell_id,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  if (!ok || !shell_id.has_value() || shell_id->empty()) {
    // The service's code (BINARY_NOT_FOUND, INVALID_CWD, …) is a param;
    // the rejected code is the tool's own, declared one.
    std::move(done).Reject(
        base::StrCat({tool_label, "_failed"}),
        ServiceFailureParams(error_code, error_message, install_hint));
    return;
  }
  PollForegroundShell(std::move(done), *shell_id, std::move(original_command),
                      start_time, std::move(tool_label));
}

void ProcessTool::PollForegroundShell(ToolCallback done,
                                      std::string shell_id,
                                      std::string original_command,
                                      base::TimeTicks start_time,
                                      std::string tool_label) {
  if (!launcher_) {
    std::move(done).Reject(kProcessUnavailableCode, base::DictValue());
    return;
  }
  launcher_->GetService()->ReadBackgroundOutput(
      shell_id,
      base::BindOnce(&ProcessTool::OnForegroundShellStatus,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done), shell_id,
                     std::move(original_command), start_time,
                     std::move(tool_label)));
}

void ProcessTool::OnForegroundShellStatus(
    ToolCallback done,
    std::string shell_id,
    std::string original_command,
    base::TimeTicks start_time,
    std::string tool_label,
    mojom::ShellBackgroundOutputPtr output) {
  if (!output || !output->found) {
    base::DictValue params;
    params.Set("reason", "shell_disappeared");
    std::move(done).Reject(base::StrCat({tool_label, "_failed"}),
                           std::move(params));
    return;
  }

  std::string stdout_text =
      BigBufferToString(output->stdout_buffer, kMaxOutputBytes);
  std::string stderr_text =
      BigBufferToString(output->stderr_buffer, kMaxOutputBytes);
  if (output->running) {
    std::string combined = stdout_text;
    if (!stderr_text.empty()) {
      if (!combined.empty()) {
        combined.push_back('\n');
      }
      combined.append(stderr_text);
    }
    if (ToolSession* session = GetSession()) {
      base::DictValue progress;
      progress.Set("kind", "shell_output");
      progress.Set("tool", tool_label);
      progress.Set("shell_id", shell_id);
      progress.Set("output", combined);
      progress.Set("stdout", stdout_text);
      progress.Set("stderr", stderr_text);
      progress.Set("stdout_truncated", output->stdout_truncated);
      progress.Set("stderr_truncated", output->stderr_truncated);
      progress.Set("elapsed_time_seconds",
                   (base::TimeTicks::Now() - start_time).InSecondsF());
      session->EmitToolProgress(base::Value(std::move(progress)));
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ProcessTool::PollForegroundShell,
                       weak_ptr_factory_.GetWeakPtr(), std::move(done), shell_id,
                       std::move(original_command), start_time,
                       std::move(tool_label)),
        base::Milliseconds(500));
    return;
  }

  auto result = mojom::ShellExecResult::New();
  result->exit = std::move(output->exit);
  result->stdout_buffer = std::move(output->stdout_buffer);
  result->stderr_buffer = std::move(output->stderr_buffer);
  result->stdout_truncated = output->stdout_truncated;
  result->stderr_truncated = output->stderr_truncated;
  OnBashExecDone(std::move(done), std::move(original_command), start_time,
                 std::move(tool_label), /*ok=*/true, std::move(result),
                 /*error_code=*/std::nullopt, /*error_message=*/std::nullopt,
                 /*install_hint=*/std::nullopt);
}

// =============================================================================
// RunPowerShellOnUI override (Windows model-facing PowerShell tool)
// =============================================================================

void ProcessTool::RunPowerShellOnUI(::nexus::generated::ExecPowerShellInput input,
                                    ToolCallback done) {
  if (input.command.empty()) {
    base::DictValue params;
    params.Set("reason", "command_empty");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }
  if (input.run_in_background.value_or(false)) {
    std::move(done).Reject(kBackgroundUnsupportedCode, base::DictValue());
    return;
  }

  ToolSession* session = GetSession();
  // Per-call cwd override: a worktree-isolated subagent sets `cwd` so its
  // commands run in its own checkout instead of the one shared session cwd
  // (parallel-safe — no race on a single session value). Absolute only,
  // mirroring ToolSession::set_cwd. Absent → fall back to the session cwd
  // (the default path for the main agent).
  base::FilePath cwd;
  if (input.cwd.has_value() && !input.cwd->empty()) {
    cwd = base::FilePath::FromUTF8Unsafe(*input.cwd);
    if (!cwd.IsAbsolute()) {
      base::DictValue params;
      params.Set("reason", "cwd_not_absolute");
      std::move(done).Reject("invalid_args", std::move(params));
      return;
    }
  } else {
    cwd = session ? session->cwd() : base::FilePath();
  }
  if (cwd.empty()) {
    base::DictValue params;
    params.Set("reason", "no_cwd");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  int timeout_ms = input.timeout.value_or(kDefaultBashTimeoutMs);
  if (timeout_ms <= 0) {
    timeout_ms = kDefaultBashTimeoutMs;
  } else if (timeout_ms > kMaxBashTimeoutMs) {
    timeout_ms = kMaxBashTimeoutMs;
  }

  // Prefer PowerShell 7 (pwsh); OnPowerShellForegroundSpawnDone falls back to
  // Windows PowerShell (powershell) if pwsh isn't installed.
  LaunchPowerShellAttempt(input.command, std::move(cwd), timeout_ms, "pwsh",
                          std::move(done));
}

void ProcessTool::LaunchPowerShellAttempt(std::string command,
                                          base::FilePath cwd,
                                          int timeout_ms,
                                          std::string shell_id,
                                          ToolCallback done) {
  if (!launcher_) {
    std::move(done).Reject(kProcessUnavailableCode, base::DictValue());
    return;
  }

  // <shell_id> -NoProfile -NonInteractive -Command <command>, invoked
  // directly (use_shell=false) — same builder as the command-hook path.
  ShellInvocation inv = BuildHookShellInvocation(shell_id, command);

  auto options = mojom::ShellSpawnOptions::New();
  options->command = inv.command;
  options->args = std::move(inv.args);
  options->cwd = cwd;
  options->use_shell = false;
  options->timeout_ms = static_cast<uint32_t>(timeout_ms);
  options->max_output_bytes = static_cast<uint32_t>(kMaxOutputBytes);

  mojo::Remote<mojom::NexusShellService>& remote = launcher_->GetService();

  remote->Spawn(
      std::move(options),
      base::BindOnce(&ProcessTool::OnPowerShellForegroundSpawnDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done), command,
                     std::move(cwd), timeout_ms, shell_id));
}

void ProcessTool::OnPowerShellExecDone(
    ToolCallback done,
    std::string command,
    base::FilePath cwd,
    int timeout_ms,
    std::string attempted_shell,
    base::TimeTicks start_time,
    bool ok,
    mojom::ShellExecResultPtr result,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  // pwsh not installed → fall back to Windows PowerShell (powershell.exe),
  // which is universally present on Windows. Only retry the pre-launch
  // resolution failures; a command that ran (any exit code) is final.
  if (!ok && attempted_shell == "pwsh" && error_code.has_value() &&
      (*error_code == "BINARY_NOT_FOUND" ||
       *error_code == "COMMAND_NOT_ALLOWED")) {
    LaunchPowerShellAttempt(std::move(command), std::move(cwd), timeout_ms,
                            "powershell", std::move(done));
    return;
  }

  // Otherwise shape exactly like the Bash tool's prose path, but label
  // rejections "powershell_failed" / "powershell_timeout" so a failure
  // from this tool isn't misreported as a Bash failure.
  OnBashExecDone(std::move(done), std::move(command), start_time, "powershell",
                 ok, std::move(result), error_code, error_message,
                 install_hint);
}


void ProcessTool::OnPowerShellForegroundSpawnDone(
    ToolCallback done,
    std::string command,
    base::FilePath cwd,
    int timeout_ms,
    std::string attempted_shell,
    bool ok,
    const std::optional<std::string>& shell_id,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  if (!ok && attempted_shell == "pwsh" && error_code.has_value() &&
      (*error_code == "BINARY_NOT_FOUND" ||
       *error_code == "COMMAND_NOT_ALLOWED")) {
    LaunchPowerShellAttempt(std::move(command), std::move(cwd), timeout_ms,
                            "powershell", std::move(done));
    return;
  }
  OnForegroundSpawnDone(std::move(done), std::move(command), base::TimeTicks::Now(),
                        "powershell", ok, shell_id, error_code, error_message,
                        install_hint);
}

void ProcessTool::OnBashExecDone(
    ToolCallback done,
    std::string original_command,
    base::TimeTicks start_time,
    std::string tool_label,
    bool ok,
    mojom::ShellExecResultPtr result,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  base::TimeDelta wall = base::TimeTicks::Now() - start_time;
  (void)wall;  // Reserved for telemetry; currently unused in prose path.
  (void)original_command;

  if (!ok || !result) {
    // Pre-launch failure (BINARY_NOT_FOUND, COMMAND_NOT_ALLOWED,
    // CWD_INVALID, …): a tool error carrying the runner's diagnostic and,
    // when present, the manual-install hint as params.
    std::move(done).Reject(
        base::StrCat({tool_label, "_failed"}),
        ServiceFailureParams(error_code, error_message, install_hint));
    return;
  }

  // Post-launch path: the child ran (success or non-zero exit) and
  // `result` carries the captured streams. Each stream is sanitised to
  // UTF-8 by BigBufferToString; the guard below is belt-and-braces since
  // a non-UTF-8 byte in a base::Value string DCHECKs.
  std::string stdout_text =
      BigBufferToString(result->stdout_buffer, kMaxOutputBytes);
  std::string stderr_text =
      BigBufferToString(result->stderr_buffer, kMaxOutputBytes);
  // `exit` is a nullable mojo struct — the foreground-poll finished path can
  // synthesize a result whose exit was moved from a null `output->exit`.
  // Every other consumer in this file guards it; do the same here.
  const bool timed_out = result->exit && result->exit->timed_out;
  int exit_code = (result->exit && result->exit->code.has_value())
                      ? *result->exit->code
                      : -1;

  std::string base = ExtractBaseCommand(original_command);
  CommandInterpretation interp = InterpretExit(base, exit_code);
  bool is_error = timed_out || interp.is_error;

  // The structured result; the runtime composes the text the model reads.
  if (!base::IsStringUTF8AllowingNoncharacters(stdout_text)) {
    stdout_text = base::UTF16ToUTF8(base::UTF8ToUTF16(stdout_text));
  }
  if (!base::IsStringUTF8AllowingNoncharacters(stderr_text)) {
    stderr_text = base::UTF16ToUTF8(base::UTF8ToUTF16(stderr_text));
  }
  base::DictValue body;
  body.Set("stdout", std::move(stdout_text));
  body.Set("stderr", std::move(stderr_text));
  body.Set("exit_code", exit_code);
  body.Set("timed_out", timed_out);
  body.Set("truncated",
           result->stdout_truncated || result->stderr_truncated);
  if (!interp.message.empty() && is_error) {
    body.Set("interpretation", interp.message);
  }

  // Failures Reject with the same structured params under
  // `<label>_timeout` / `<label>_failed`; the runtime marks the tool
  // result as an error and renders the body text.
  if (is_error) {
    std::string code =
        base::StrCat({tool_label, timed_out ? "_timeout" : "_failed"});
    std::move(done).Reject(std::move(code), std::move(body));
    return;
  }
  std::move(done).Resolve(std::move(body));
}

void ProcessTool::CompleteExecForTesting(
    ToolCallback done,
    const std::string& tool_label,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  OnBashExecDone(std::move(done), std::string(), base::TimeTicks::Now(),
                 tool_label, /*ok=*/false, /*result=*/nullptr, error_code,
                 error_message, install_hint);
}

void ProcessTool::CompleteGrepForTesting(ToolCallback done,
                                         RipgrepRunResult result) {
  OnGrepRgDone(std::move(done), "content", /*head_limit=*/0, /*offset=*/0,
               base::FilePath(), std::move(result));
}

void ProcessTool::CompleteHookExecForTesting(
    ToolCallback done,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  OnHookExecDone(std::move(done), /*ok=*/false, /*result=*/nullptr,
                 error_code, error_message, install_hint);
}

// =============================================================================
// OnHookExecDone — raw-result callback for the hook-exec path
// =============================================================================

void ProcessTool::OnHookExecDone(
    ToolCallback done,
    bool ok,
    mojom::ShellExecResultPtr result,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  // Pre-launch failure: the requested shell isn't on the host
  // (BINARY_NOT_FOUND), the command is disallowed, or the cwd is invalid.
  // Reject so the JS hook runner can detect it — when its shell preference
  // was the default bash list, it retries with PowerShell. install_hint is
  // appended when present.
  if (!ok || !result) {
    std::move(done).Reject(
        "hook_exec_failed",
        ServiceFailureParams(error_code, error_message, install_hint));
    return;
  }

  // Post-launch: return the UNSHAPED exit code + combined stdout/stderr.
  // Unlike RunBashOnUI's prose path, we deliberately do NOT reject on a
  // non-zero exit — the JS hook executor owns the exit-code protocol
  // (2 = block, 0 = pass, other = non-blocking warning), so it needs the
  // raw code and output. The JS `readBashResult` reads {output, exit_code}.
  std::string stdout_text =
      BigBufferToString(result->stdout_buffer, kMaxOutputBytes);
  std::string stderr_text =
      BigBufferToString(result->stderr_buffer, kMaxOutputBytes);
  // `exit` is nullable — guard like every other consumer in this file.
  int exit_code = (result->exit && result->exit->code.has_value())
                      ? *result->exit->code
                      : -1;
  if (result->exit && result->exit->timed_out) {
    exit_code = -1;
  }

  std::string combined = stdout_text;
  if (!stderr_text.empty()) {
    if (!combined.empty()) {
      combined.push_back('\n');
    }
    combined.append(stderr_text);
  }

  // This tree aliases the dictionary type to base::DictValue (see
  // base/values.h); base::Value has no nested ::Dict here. `done.Resolve`
  // accepts a DictValue directly (same as browser_tool.cc).
  base::DictValue dict;
  dict.Set("output", std::move(combined));
  dict.Set("exit_code", exit_code);
  std::move(done).Resolve(std::move(dict));
}

// =============================================================================
// RunGrepOnUI override
// =============================================================================

void ProcessTool::RunGrepOnUI(::nexus::generated::FilesGrepInput input,
                              ToolCallback done) {
  if (input.pattern.empty()) {
    base::DictValue params;
    params.Set("reason", "pattern_empty");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  ToolSession* session = GetSession();
  base::FilePath default_root = session ? session->cwd() : base::FilePath();
  base::FilePath target =
      ResolveAgainstCwd(input.path.value_or(""), default_root);
  if (target.empty()) {
    base::DictValue params;
    params.Set("reason", "no_cwd");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  // Normalize output_mode; reject unknown values rather than silently
  // falling back, since the model just hallucinated otherwise. Done before
  // the (blocking) path check so invalid_args surfaces without a thread hop.
  std::string output_mode = input.output_mode.value_or("files_with_matches");
  if (output_mode != "content" && output_mode != "files_with_matches" &&
      output_mode != "count") {
    base::DictValue params;
    params.Set("reason", "output_mode");
    params.Set("value", output_mode);
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  bool show_line_numbers = input.show_line_numbers.value_or(true);
  int head_limit =
      input.head_limit.has_value() ? *input.head_limit : kDefaultGrepHeadLimit;
  if (head_limit < 0) {
    head_limit = kDefaultGrepHeadLimit;
  }
  int offset = std::max(0, input.offset.value_or(0));

  RipgrepRunOptions options;
  // rg: cwd doesn't drive matching (the target is the trailing argv) but the
  // shell service requires an absolute, existing dir.
  options.cwd = default_root.empty() ? target : default_root;
  options.timeout_ms = static_cast<uint32_t>(kDefaultBashTimeoutMs);
  options.max_output_bytes = static_cast<uint32_t>(kMaxOutputBytes);

  if (!launcher_) {
    std::move(done).Reject(kProcessUnavailableCode, base::DictValue());
    return;
  }

  // Spec 2-4-4: validate that the target path exists. Skip the check
  // for UNC paths to avoid NTLM credential leaks —
  // treat them as existing. base::PathExists is blocking I/O, so it must run
  // on a ThreadPool worker, never on this UI thread (mirrors FileTool's
  // PostTaskAndReplyWithResult idiom).
  std::string target_utf8 = target.AsUTF8Unsafe();
  bool is_unc =
      base::StartsWith(target_utf8, "\\\\", base::CompareCase::SENSITIVE) ||
      base::StartsWith(target_utf8, "//", base::CompareCase::SENSITIVE);
  if (is_unc) {
    OnGrepPathChecked(std::move(input), std::move(done),
                      std::move(output_mode), show_line_numbers, head_limit,
                      offset, target, default_root, std::move(options),
                      GrepTargetKind::kUnknown);
    return;
  }

  base::FilePath target_for_check = target;
  BlockingRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ProcessTool::ClassifyGrepTargetOnBlocking,
                     std::move(target_for_check)),
      base::BindOnce(&ProcessTool::OnGrepPathChecked,
                     weak_ptr_factory_.GetWeakPtr(), std::move(input),
                     std::move(done), std::move(output_mode), show_line_numbers,
                     head_limit, offset, std::move(target),
                     std::move(default_root), std::move(options)));
}

ProcessTool::GrepTargetKind ProcessTool::ClassifyGrepTargetOnBlocking(
    base::FilePath target) {
  base::File::Info info;
  if (!base::GetFileInfo(target, &info)) {
    return GrepTargetKind::kMissing;
  }
  return info.is_directory ? GrepTargetKind::kDirectory
                           : GrepTargetKind::kFile;
}

void ProcessTool::OnGrepPathChecked(::nexus::generated::FilesGrepInput input,
                                    ToolCallback done,
                                    std::string output_mode,
                                    bool show_line_numbers,
                                    int head_limit,
                                    int offset,
                                    base::FilePath target,
                                    base::FilePath default_root,
                                    RipgrepRunOptions options,
                                    GrepTargetKind target_kind) {
  if (target_kind == GrepTargetKind::kMissing) {
    base::DictValue params;
    params.Set("path", input.path.value_or(""));
    std::move(done).Reject("path_not_found", std::move(params));
    return;
  }

  ApplySingleFileGrepDefaults(input, output_mode,
                              target_kind == GrepTargetKind::kFile);
  std::vector<std::string> rg_args =
      BuildRipgrepArgs(input, output_mode, show_line_numbers);
  rg_args.push_back(target.AsUTF8Unsafe());

  // The runner owns the exec + BINARY_NOT_FOUND → auto-install rg → retry
  // flow (shared with FileTool::Glob); we only shape its parsed lines.
  ripgrep_runner_.Run(
      launcher_.get(), GetProfile(), std::move(rg_args), std::move(options),
      base::BindOnce(&ProcessTool::OnGrepRgDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done), std::move(output_mode), head_limit,
                     offset, default_root));
}

void ProcessTool::OnGrepRgDone(ToolCallback done,
                               std::string output_mode,
                               int head_limit,
                               int offset,
                               base::FilePath cwd,
                               RipgrepRunResult result) {
  // Pre-launch failure: rg was unavailable and the runner's one-shot
  // auto-install didn't recover it. The runner already shaped a diagnostic
  // message (+ manual-install hint); surface it under a Grep-labeled code.
  if (!result.launched) {
    std::move(done).Reject(
        "grep_failed",
        ServiceFailureParams(result.error_code, result.error_message,
                             std::nullopt));
    return;
  }

  std::vector<std::string> lines = std::move(result.lines);

  // Structured per-mode results; the runtime renders the text the model
  // reads (lines, totals, pagination note).

  if (output_mode == "content") {
    SliceResult sliced = ApplyHeadLimit(std::move(lines), head_limit, offset);
    // Relativize file paths in content lines (first colon separator).
    for (auto& line : sliced.items) {
      line = RelativizeGrepLine(line, cwd, /*use_last_colon=*/false);
    }
    base::ListValue items;
    for (std::string& line : sliced.items) {
      items.Append(std::move(line));
    }
    base::DictValue body;
    body.Set("mode", "content");
    body.Set("lines", std::move(items));
    body.Set("truncated", sliced.truncated);
    body.Set("limit", sliced.applied_limit);
    body.Set("offset", offset);
    std::move(done).Resolve(std::move(body));
    return;
  }

  if (output_mode == "count") {
    SliceResult sliced = ApplyHeadLimit(std::move(lines), head_limit, offset);
    int total_matches = 0;
    int file_count = 0;
    // Relativize file paths in count lines (last colon separator).
    for (auto& line : sliced.items) {
      line = RelativizeGrepLine(line, cwd, /*use_last_colon=*/true);
      // Parse count from the relativized line.
      size_t colon = line.find_last_of(':');
      if (colon == std::string::npos || colon == 0) {
        continue;
      }
      int count = 0;
      if (base::StringToInt(line.substr(colon + 1), &count)) {
        total_matches += count;
        ++file_count;
      }
    }
    base::ListValue items;
    for (std::string& line : sliced.items) {
      items.Append(std::move(line));
    }
    base::DictValue body;
    body.Set("mode", "count");
    body.Set("lines", std::move(items));
    body.Set("total_matches", total_matches);
    body.Set("file_count", file_count);
    body.Set("truncated", sliced.truncated);
    body.Set("limit", sliced.applied_limit);
    body.Set("offset", offset);
    std::move(done).Resolve(std::move(body));
    return;
  }
  // files_with_matches: rg already returns one path per line.
  // Sort by mtime descending before applying head_limit. The sort
  // stats every matched file (base::GetFileInfo — blocking I/O), so it must
  // run on a ThreadPool worker, never on this UI thread. The worker shapes the
  // full body; OnGrepFilesShaped just resolves `done` back on UI.
  BlockingRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ProcessTool::ShapeGrepFilesWithMatchesBlocking,
                     std::move(lines), head_limit, offset, std::move(cwd)),
      base::BindOnce(&ProcessTool::OnGrepFilesShaped,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done)));
}

// static
base::DictValue ProcessTool::ShapeGrepFilesWithMatchesBlocking(
    std::vector<std::string> lines,
    int head_limit,
    int offset,
    base::FilePath cwd) {
  SortByMtimeDesc(&lines);
  SliceResult sliced = ApplyHeadLimit(std::move(lines), head_limit, offset);
  // Relativize all file paths.
  for (auto& path : sliced.items) {
    path = ToRelativePath(base::FilePath::FromUTF8Unsafe(path), cwd);
  }
  base::ListValue files;
  for (std::string& path : sliced.items) {
    files.Append(std::move(path));
  }
  base::DictValue body;
  body.Set("mode", "files");
  body.Set("files", std::move(files));
  body.Set("truncated", sliced.truncated);
  body.Set("limit", sliced.applied_limit);
  body.Set("offset", offset);
  return body;
}

void ProcessTool::OnGrepFilesShaped(ToolCallback done, base::DictValue body) {
  std::move(done).Resolve(std::move(body));
}

}  // namespace nexus


// =============================================================================
// Test-only static wrappers — thin shims around the anonymous-namespace
// helpers so process_tool_unittest.cc can pin spec 2-4-5 / 2-4-6 without
// a live utility process.
// =============================================================================

// static
std::vector<std::string> nexus::ProcessTool::BuildRipgrepArgsForTesting(
    const ::nexus::generated::FilesGrepInput& input,
    const std::string& output_mode,
    bool show_line_numbers) {
  return BuildRipgrepArgs(input, output_mode, show_line_numbers);
}

// static
void nexus::ProcessTool::ApplySingleFileGrepDefaultsForTesting(
    ::nexus::generated::FilesGrepInput& input,
    std::string& output_mode,
    bool target_is_file) {
  ApplySingleFileGrepDefaults(input, output_mode, target_is_file);
}

// static
std::vector<std::string> nexus::ProcessTool::SplitGlobPatternsForTesting(
    const std::string& glob) {
  return SplitGlobPatterns(glob);
}
