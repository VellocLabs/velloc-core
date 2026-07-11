#include "custom_browser/browser/nexus/tools/process_tool/process_tool.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "build/build_config.h"
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
  // Windows: run git-bash directly. The Bash tool is only surfaced on
  // Windows when `bash` resolves on PATH (see the listHostedTools filter in
  // nexus_catalog_bridge_adapter.cc); on a no-bash host the model gets the
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
// pwsh/powershell use the non-interactive `-Command` form (mirrors
// claude_code's execCommandHook PowerShell argv:
// `-NoProfile -NonInteractive -Command`). The shell id is resolved by the
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

// Decode a base::BigBuffer into a UTF-8 std::string capped at
// `kMaxOutputBytes`. Invalid UTF-8 is preserved byte-for-byte (a tool
// returning binary on stdout shouldn't crash the bridge).
std::string BigBufferToString(const mojo_base::BigBuffer& buffer,
                              size_t cap) {
  base::span<const uint8_t> bytes = buffer.byte_span();
  size_t n = std::min(bytes.size(), cap);
  return std::string(reinterpret_cast<const char*>(bytes.data()), n);
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
  std::string segment = (boundary == std::string::npos)
                            ? command
                            : command.substr(boundary);
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
  if (first.size() >= 2 &&
      (first.front() == '\'' || first.front() == '"') &&
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
    return {exit >= 2,
            exit == 1 ? "No matches found" : ""};
  }
  if (base == "find") {
    return {exit >= 2,
            exit == 1 ? "Some directories were inaccessible" : ""};
  }
  if (base == "diff") {
    return {exit >= 2,
            exit == 1 ? "Files differ" : ""};
  }
  if (base == "test" || base == "[") {
    return {exit >= 2,
            exit == 1 ? "Condition is false" : ""};
  }
  // Default semantic.
  return {exit != 0,
          exit != 0
              ? base::StrCat({"Command failed with exit code ",
                              base::NumberToString(exit)})
              : ""};
}

// =============================================================================
// Grep helpers — port of TS GrepTool's argv-shaping + result-shaping
// =============================================================================

constexpr int kDefaultGrepHeadLimit = 250;
constexpr int kGrepMaxColumns = 500;

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
  for (const std::string& part :
       base::SplitString(glob, " \t", base::TRIM_WHITESPACE,
                          base::SPLIT_WANT_NONEMPTY)) {
    if (part.find('{') != std::string::npos &&
        part.find('}') != std::string::npos) {
      out.push_back(part);
      continue;
    }
    for (const std::string& inner :
         base::SplitString(part, ",", base::TRIM_WHITESPACE,
                            base::SPLIT_WANT_NONEMPTY)) {
      out.push_back(inner);
    }
  }
  return out;
}

// Build the rg argv for a Grep call. Mirrors `buildRipgrepArgs` from
// the retired GrepTool.ts. `pattern` is the user's regex.
std::vector<std::string> BuildRipgrepArgs(
    const ::nexus::generated::GrepInput& input,
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
      if (input.before_context.has_value()) {
        args.push_back("-B");
        args.push_back(base::NumberToString(*input.before_context));
      }
      if (input.after_context.has_value()) {
        args.push_back("-A");
        args.push_back(base::NumberToString(*input.after_context));
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

// Apply head_limit + offset slicing matching the TS `applyHeadLimit`.
// Returns (sliced, applied_limit_when_truncated, true_if_truncated).
struct SliceResult {
  std::vector<std::string> items;
  int applied_limit = 0;     // 0 means "not truncated / no limit reported"
  bool truncated = false;
};

SliceResult ApplyHeadLimit(std::vector<std::string>&& items,
                           int head_limit, int offset) {
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

// Parse rg's stdout into a vector of trimmed, CRLF-stripped lines.
// rg exits with 1 when there are no matches — that's success from the
// caller's POV (we return an empty vector). Any other non-zero exit
// is a real error; we still return whatever rg printed (best-effort).
std::vector<std::string> ParseRipgrepLines(
    const mojo_base::BigBuffer& stdout_buffer,
    int exit_code,
    size_t cap) {
  if (exit_code == 1) {
    return {};
  }
  base::span<const uint8_t> bytes = stdout_buffer.byte_span();
  size_t n = std::min(bytes.size(), cap);
  std::string text(reinterpret_cast<const char*>(bytes.data()), n);
  std::string trimmed;
  base::TrimString(text, "\r\n \t", &trimmed);
  if (trimmed.empty()) {
    return {};
  }
  std::vector<std::string> lines;
  for (const std::string& line :
       base::SplitString(trimmed, "\n", base::TRIM_WHITESPACE,
                          base::SPLIT_WANT_NONEMPTY)) {
    // Strip lone trailing \r (in case rg printed CRLF on Windows).
    if (!line.empty() && line.back() == '\r') {
      lines.push_back(line.substr(0, line.size() - 1));
    } else {
      lines.push_back(line);
    }
  }
  return lines;
}

}  // namespace

// =============================================================================
// Construction / lifecycle
// =============================================================================

// static
void ProcessTool::DefineTools(ToolRegistry& /*registry*/) {
  // Body intentionally empty. All process-tool registration flows
  // through `generated::PopulateRegistry()`.
}

ProcessTool::ProcessTool(ToolSession* session, Profile* profile,
                         const std::string& session_key)
    : ToolBase(session, profile, session_key),
      launcher_(std::make_unique<NexusShellServiceLauncher>()) {}

ProcessTool::~ProcessTool() {
  // Defensive — invalidate WeakPtrs so any pending mojo callback drops
  // cleanly, then drop the launcher (which releases the Remote and
  // disconnects the utility process).
  OnStop(OnStopReason::kDestructor);
}

void ProcessTool::OnStop(OnStopReason reason) {
  LOG(INFO) << "[ProcessTool] OnStop session=" << GetSessionKey()
            << " reason=" << OnStopReasonToString(reason);
  // Gated on TEARDOWN reasons only — see the matching comment in
  // FileTool::OnStop for the full rationale. Summary: after the typed-
  // OnStop refactor, kCompleted / kUserStopped fire while the
  // ToolSession stays warm; invalidating WeakPtrs and shutting down
  // the utility process here would break every subsequent Bash call
  // in the same session (the cached weak_ptr captured by the next
  // mojo callback would be dead, and the launcher's utility-process
  // Remote would be torn down). Only invalidate / shut down when the
  // ProcessTool is actually going away.
  const bool teardown = reason == OnStopReason::kSessionDeleted ||
                        reason == OnStopReason::kDisconnected ||
                        reason == OnStopReason::kShutdown ||
                        reason == OnStopReason::kDestructor;
  if (teardown) {
    weak_ptr_factory_.InvalidateWeakPtrs();
    if (launcher_) {
      launcher_->Shutdown();
    }
  }
}

// =============================================================================
// RunBashOnUI override
// =============================================================================

void ProcessTool::RunBashOnUI(::nexus::generated::BashInput input,
                              ToolCallback done) {
  // The dispatcher already validated input shape (command is required
  // string). We add semantic validation here.
  if (input.command.empty()) {
    std::move(done).Reject("invalid_args", "command must not be empty");
    return;
  }

  ToolSession* session = GetSession();
  base::FilePath cwd = session ? session->cwd() : base::FilePath();
  if (cwd.empty()) {
    std::move(done).Reject(
        "invalid_args",
        "session cwd is not set. Push a cwd via setAgentLoopSettings "
        "before running shell commands.");
    return;
  }

  int timeout_ms = input.timeout.value_or(kDefaultBashTimeoutMs);
  if (timeout_ms <= 0) {
    timeout_ms = kDefaultBashTimeoutMs;
  } else if (timeout_ms > kMaxBashTimeoutMs) {
    timeout_ms = kMaxBashTimeoutMs;
  }

  // Hook-exec path: the internal `hook_shell` field (set only by the JS
  // hook runner, never the model) requests an explicit shell with hook
  // input on stdin and env exported natively — the CC-faithful command-hook
  // contract. Run the requested shell directly (use_shell = false) and
  // return the RAW exit code + combined output so the JS executor can apply
  // its own protocol (exit 2 = block, 0 = pass, other = non-blocking
  // warning). This bypasses the prose/non-zero-reject shaping of the normal
  // Bash path.
  if (input.hook_shell.has_value() && !input.hook_shell->empty()) {
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

  mojo::Remote<mojom::NexusShellService>& remote = launcher_->GetService();
  remote->Exec(
      std::move(options),
      base::BindOnce(&ProcessTool::OnBashExecDone,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(done), input.command,
                     base::TimeTicks::Now()));
}

// =============================================================================
// RunPowerShellOnUI override (Windows model-facing PowerShell tool)
// =============================================================================

void ProcessTool::RunPowerShellOnUI(
    ::nexus::generated::PowerShellInput input, ToolCallback done) {
  if (input.command.empty()) {
    std::move(done).Reject("invalid_args", "command must not be empty");
    return;
  }

  ToolSession* session = GetSession();
  base::FilePath cwd = session ? session->cwd() : base::FilePath();
  if (cwd.empty()) {
    std::move(done).Reject(
        "invalid_args",
        "session cwd is not set. Push a cwd via setAgentLoopSettings "
        "before running shell commands.");
    return;
  }

  int timeout_ms = input.timeout.value_or(kDefaultBashTimeoutMs);
  if (timeout_ms <= 0) {
    timeout_ms = kDefaultBashTimeoutMs;
  } else if (timeout_ms > kMaxBashTimeoutMs) {
    timeout_ms = kMaxBashTimeoutMs;
  }

  // Prefer PowerShell 7 (pwsh); OnPowerShellExecDone falls back to Windows
  // PowerShell (powershell) if pwsh isn't installed.
  LaunchPowerShellAttempt(input.command, std::move(cwd), timeout_ms, "pwsh",
                          std::move(done));
}

void ProcessTool::LaunchPowerShellAttempt(std::string command,
                                          base::FilePath cwd,
                                          int timeout_ms,
                                          std::string shell_id,
                                          ToolCallback done) {
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
  remote->Exec(
      std::move(options),
      base::BindOnce(&ProcessTool::OnPowerShellExecDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done), command,
                     std::move(cwd), timeout_ms, shell_id,
                     base::TimeTicks::Now()));
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

  // Otherwise shape exactly like the Bash tool's prose path.
  OnBashExecDone(std::move(done), std::move(command), start_time, ok,
                 std::move(result), error_code, error_message, install_hint);
}

void ProcessTool::OnBashExecDone(
    ToolCallback done,
    std::string original_command,
    base::TimeTicks start_time,
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
    // CWD_INVALID, …). CC parity: surface as a tool error via
    // done.Reject — the framework marks the tool_result as an error,
    // matching how CC's BashTool emits failures. install_hint, when
    // present, is appended to the message so the model has a recovery
    // suggestion inline.
    std::string msg =
        error_message.has_value() ? *error_message : std::string();
    if (install_hint.has_value() && !install_hint->empty()) {
      if (!msg.empty()) {
        msg.append("\n\n");
      }
      msg.append(*install_hint);
    }
    std::string code = error_code.value_or("bash_failed");
    std::move(done).Reject(std::move(code), std::move(msg));
    return;
  }

  // Post-launch path — the child ran (success or non-zero exit), and
  // `result` carries the captured streams. CC parity: emit
  // [stdout, stderr].filter(Boolean).join('\n') as the model-visible
  // result string. Non-zero exit codes get a leading annotation line
  // so the model knows the command failed, mirroring CC's behavior of
  // surfacing exit_code via is_error.
  std::string stdout_text =
      BigBufferToString(result->stdout_buffer, kMaxOutputBytes);
  std::string stderr_text =
      BigBufferToString(result->stderr_buffer, kMaxOutputBytes);
  int exit_code = result->exit->code.has_value()
                      ? *result->exit->code
                      : -1;

  std::string base = ExtractBaseCommand(original_command);
  CommandInterpretation interp = InterpretExit(base, exit_code);
  bool is_error = result->exit->timed_out || interp.is_error;

  // Build the prose body.
  std::string body;
  if (result->exit->timed_out) {
    body.append("Command timed out.\n");
  } else if (exit_code != 0) {
    body.append(base::StrCat({"Command exited with code ",
                              base::NumberToString(exit_code), ".\n"}));
  }
  if (!stdout_text.empty()) {
    body.append(stdout_text);
  }
  if (!stderr_text.empty()) {
    if (!stdout_text.empty()) {
      body.push_back('\n');
    }
    body.append(stderr_text);
  }
  if (result->stdout_truncated || result->stderr_truncated) {
    if (!body.empty() && body.back() != '\n') body.push_back('\n');
    body.append(
        "[output truncated — re-run with a narrower scope to capture more]");
  }
  if (!interp.message.empty() && is_error) {
    if (!body.empty() && body.back() != '\n') body.push_back('\n');
    body.append(interp.message);
  }
  if (body.empty()) {
    body = "(no output)";
  }

  // Failures Reject so the framework marks the tool_result as
  // isError, matching CC's BashTool which sets is_error: true when
  // exit_code != 0 / timed_out.
  if (is_error) {
    std::string code = result->exit->timed_out ? "bash_timeout" : "bash_failed";
    std::move(done).Reject(std::move(code), std::move(body));
    return;
  }
  std::move(done).Resolve(base::Value(std::move(body)));
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
    std::string msg = error_message.value_or(std::string());
    if (install_hint.has_value() && !install_hint->empty()) {
      if (!msg.empty()) {
        msg.append("\n\n");
      }
      msg.append(*install_hint);
    }
    std::move(done).Reject(error_code.value_or("hook_exec_failed"),
                           std::move(msg));
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
  int exit_code = result->exit->code.has_value() ? *result->exit->code : -1;
  if (result->exit->timed_out) {
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

void ProcessTool::RunGrepOnUI(::nexus::generated::GrepInput input,
                              ToolCallback done) {
  if (input.pattern.empty()) {
    std::move(done).Reject("invalid_args", "pattern must not be empty");
    return;
  }

  ToolSession* session = GetSession();
  base::FilePath default_root = session ? session->cwd() : base::FilePath();
  base::FilePath target =
      ResolveAgainstCwd(input.path.value_or(""), default_root);
  if (target.empty()) {
    std::move(done).Reject(
        "invalid_args",
        "session cwd is not set and no absolute `path` was provided. "
        "Push a cwd via setAgentLoopSettings or pass an absolute path.");
    return;
  }

  // Normalize output_mode; reject unknown values rather than silently
  // falling back, since the model just hallucinated otherwise.
  std::string output_mode =
      input.output_mode.value_or("files_with_matches");
  if (output_mode != "content" && output_mode != "files_with_matches" &&
      output_mode != "count") {
    std::move(done).Reject(
        "invalid_args",
        base::StrCat({"output_mode must be 'content', 'files_with_matches', "
                      "or 'count'; got '",
                      output_mode, "'"}));
    return;
  }

  bool show_line_numbers = input.show_line_numbers.value_or(true);
  int head_limit = input.head_limit.has_value() ? *input.head_limit
                                                : kDefaultGrepHeadLimit;
  if (head_limit < 0) {
    head_limit = kDefaultGrepHeadLimit;
  }
  int offset = std::max(0, input.offset.value_or(0));

  std::vector<std::string> rg_args =
      BuildRipgrepArgs(input, output_mode, show_line_numbers);
  rg_args.push_back(target.AsUTF8Unsafe());

  auto options = mojom::ShellSpawnOptions::New();
  options->command = "rg";
  options->args = std::move(rg_args);
  // For rg: cwd doesn't drive matching (we passed the target as the
  // last argv) but the service requires an absolute, existing dir.
  options->cwd = default_root.empty() ? target : default_root;
  options->use_shell = false;
  options->timeout_ms = kDefaultBashTimeoutMs;
  options->max_output_bytes = static_cast<uint32_t>(kMaxOutputBytes);

  mojo::Remote<mojom::NexusShellService>& remote = launcher_->GetService();
  remote->Exec(
      std::move(options),
      base::BindOnce(&ProcessTool::OnGrepExecDone,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(done), output_mode, head_limit, offset));
}

void ProcessTool::OnGrepExecDone(
    ToolCallback done,
    std::string output_mode,
    int head_limit,
    int offset,
    bool ok,
    mojom::ShellExecResultPtr result,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  // Pre-launch failure path. ripgrep missing from the host shows up
  // here as BINARY_NOT_FOUND with an `install_hint` the model can use
  // to install rg via Bash and retry. CC parity: emit as a tool
  // error so the framework marks the tool_result.is_error.
  if (!ok || !result) {
    std::string msg =
        error_message.has_value()
            ? base::StrCat({"ripgrep unavailable: ", *error_message})
            : std::string("ripgrep unavailable");
    if (install_hint.has_value() && !install_hint->empty()) {
      msg = base::StrCat({msg, "\n\n", *install_hint});
    }
    std::string code = error_code.value_or("grep_failed");
    std::move(done).Reject(std::move(code), std::move(msg));
    return;
  }

  int exit_code =
      result->exit->code.has_value() ? *result->exit->code : 0;
  std::vector<std::string> lines = ParseRipgrepLines(
      result->stdout_buffer, exit_code, kMaxOutputBytes);

  // CC parity: GrepTool.ts emits per-mode prose strings. Mirror those
  // exactly so the model's recognition pattern is shared with CC.

  if (output_mode == "content") {
    SliceResult sliced =
        ApplyHeadLimit(std::move(lines), head_limit, offset);
    if (sliced.items.empty()) {
      std::move(done).Resolve(base::Value("No files found"));
      return;
    }
    std::string body = base::JoinString(sliced.items, "\n");
    if (sliced.truncated) {
      body.append(base::StrCat(
          {"\n\n[Showing results with pagination = limit ",
           base::NumberToString(sliced.applied_limit),
           offset > 0 ? base::StrCat(
                            {", offset ", base::NumberToString(offset)})
                      : std::string(),
           "]"}));
    }
    std::move(done).Resolve(base::Value(std::move(body)));
    return;
  }

  if (output_mode == "count") {
    SliceResult sliced =
        ApplyHeadLimit(std::move(lines), head_limit, offset);
    int total_matches = 0;
    int file_count = 0;
    for (const std::string& line : sliced.items) {
      // rg --count emits "path:N" per file. Take the LAST colon to
      // avoid mis-splitting Windows-style paths like "C:\foo:42".
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
    std::string body = base::JoinString(sliced.items, "\n");
    body.append(base::StrCat({"\n\nFound ", base::NumberToString(total_matches),
                              " total occurrence(s) across ",
                              base::NumberToString(file_count),
                              " file(s)."}));
    if (sliced.truncated) {
      body.append(base::StrCat(
          {" [Showing results with pagination = limit ",
           base::NumberToString(sliced.applied_limit), "]"}));
    }
    std::move(done).Resolve(base::Value(std::move(body)));
    return;
  }

  // files_with_matches: rg already returns one path per line.
  SliceResult sliced =
      ApplyHeadLimit(std::move(lines), head_limit, offset);
  if (sliced.items.empty()) {
    std::move(done).Resolve(base::Value("No files found"));
    return;
  }
  std::string body = base::StrCat({"Found ",
                                    base::NumberToString(sliced.items.size()),
                                    " file(s)"});
  if (sliced.truncated) {
    body.append(base::StrCat(
        {" [Showing results with pagination = limit ",
         base::NumberToString(sliced.applied_limit), "]"}));
  }
  body.push_back('\n');
  body.append(base::JoinString(sliced.items, "\n"));
  std::move(done).Resolve(base::Value(std::move(body)));
}

}  // namespace nexus

// Definition matching the NEXUS_DECLARE_CATEGORY in the header. Lives
// in the .cc so it can include `tool_session.h` (the header can't —
// `tool_session.h` itself includes process_tool.h to embed
// ProcessTool as a member). The dispatcher in
// nexus_tools_registry.gen.cc invokes this via
// `CategoryAccessor<ProcessTool>::Get(session)`.
NEXUS_DEFINE_CATEGORY(nexus::ProcessTool, process_tool);
