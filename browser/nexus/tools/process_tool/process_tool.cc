#include "custom_browser/browser/nexus/tools/process_tool/process_tool.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
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
  // `/d` skips AutoRun, `/s` adjusts quoting rules, `/c` runs and
  // exits — same pattern claude_code uses for cmd-on-Windows.
  inv.command = "cmd";
  inv.args = {"/d", "/s", "/c", command_line};
#else
  // Default to /bin/sh; the utility process resolves it against the
  // SafeCommandPolicy allowlist. If a host has bash but not sh the
  // policy already accepts both.
  inv.command = "sh";
  inv.args = {"-c", command_line};
#endif
  return inv;
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
  OnStop("destructor");
}

void ProcessTool::OnStop(const std::string& reason) {
  LOG(INFO) << "[ProcessTool] OnStop session=" << GetSessionKey()
            << " reason=" << reason;
  weak_ptr_factory_.InvalidateWeakPtrs();
  if (launcher_) {
    launcher_->Shutdown();
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

  ShellInvocation invocation = BuildShellInvocation(input.command);

  auto options = mojom::ShellSpawnOptions::New();
  options->command = invocation.command;
  options->args = std::move(invocation.args);
  options->cwd = cwd;
  options->use_shell = true;
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

  base::DictValue payload;
  payload.Set("command", original_command);

  if (!ok || !result) {
    // Pre-launch failure (BINARY_NOT_FOUND, COMMAND_NOT_ALLOWED,
    // CWD_INVALID, …). Surface via the standard `error_code` /
    // `error_message` pair plus optional install_hint so the model can
    // self-recover (e.g. install a missing binary then retry). Shape
    // mirrors the TS `makeFailureOutput`.
    payload.Set("stdout", "");
    std::string stderr_text =
        error_message.has_value() ? *error_message : std::string();
    if (install_hint.has_value() && !install_hint->empty()) {
      stderr_text = base::StrCat({stderr_text, "\n\n", *install_hint});
    }
    payload.Set("stderr", stderr_text);
    payload.Set("stdout_truncated", false);
    payload.Set("stderr_truncated", false);
    payload.Set("exit_code", -1);
    payload.Set("timed_out", false);
    payload.Set("duration_ms", static_cast<int>(wall.InMilliseconds()));
    payload.Set("is_error", true);
    if (error_code.has_value()) {
      payload.Set("interpretation", *error_code);
    }
    if (install_hint.has_value() && !install_hint->empty()) {
      payload.Set("install_hint", *install_hint);
    }
    std::move(done).Resolve(std::move(payload));
    return;
  }

  // Post-launch path — the child ran (success or non-zero exit), and
  // `result` carries the captured streams.
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

  payload.Set("stdout", std::move(stdout_text));
  payload.Set("stderr", std::move(stderr_text));
  payload.Set("stdout_truncated", result->stdout_truncated);
  payload.Set("stderr_truncated", result->stderr_truncated);
  payload.Set("exit_code", exit_code);
  if (result->exit->signal.has_value()) {
    payload.Set("signal", *result->exit->signal);
  }
  payload.Set("timed_out", result->exit->timed_out);
  payload.Set("duration_ms",
              static_cast<int>(result->exit->duration_ms));
  payload.Set("is_error", is_error);
  if (!interp.message.empty()) {
    payload.Set("interpretation", interp.message);
  }

  std::move(done).Resolve(std::move(payload));
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
  // to install rg via Bash and retry.
  if (!ok || !result) {
    base::DictValue payload;
    payload.Set("mode", output_mode);
    payload.Set("num_files", 0);
    base::ListValue empty_filenames;
    payload.Set("filenames", std::move(empty_filenames));
    std::string content =
        error_message.has_value()
            ? base::StrCat({"ripgrep unavailable: ", *error_message})
            : "ripgrep unavailable";
    if (install_hint.has_value() && !install_hint->empty()) {
      content = base::StrCat({content, "\n\n", *install_hint});
      payload.Set("install_hint", *install_hint);
    }
    payload.Set("content", std::move(content));
    if (error_code.has_value()) {
      payload.Set("interpretation", *error_code);
    }
    std::move(done).Resolve(std::move(payload));
    return;
  }

  int exit_code =
      result->exit->code.has_value() ? *result->exit->code : 0;
  std::vector<std::string> lines = ParseRipgrepLines(
      result->stdout_buffer, exit_code, kMaxOutputBytes);

  base::DictValue payload;
  payload.Set("mode", output_mode);

  if (output_mode == "content") {
    SliceResult sliced =
        ApplyHeadLimit(std::move(lines), head_limit, offset);
    payload.Set("num_files", 0);
    base::ListValue empty_filenames;
    payload.Set("filenames", std::move(empty_filenames));
    payload.Set("content", base::JoinString(sliced.items, "\n"));
    payload.Set("num_lines", static_cast<int>(sliced.items.size()));
    if (sliced.truncated) {
      payload.Set("applied_limit", sliced.applied_limit);
    }
    if (offset > 0) {
      payload.Set("applied_offset", offset);
    }
  } else if (output_mode == "count") {
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
    payload.Set("num_files", file_count);
    base::ListValue empty_filenames;
    payload.Set("filenames", std::move(empty_filenames));
    payload.Set("num_matches", total_matches);
    payload.Set("content", base::JoinString(sliced.items, "\n"));
    if (sliced.truncated) {
      payload.Set("applied_limit", sliced.applied_limit);
    }
    if (offset > 0) {
      payload.Set("applied_offset", offset);
    }
  } else {
    // files_with_matches: rg already returns one path per line.
    SliceResult sliced =
        ApplyHeadLimit(std::move(lines), head_limit, offset);
    base::ListValue filenames;
    for (const std::string& fp : sliced.items) {
      filenames.Append(fp);
    }
    payload.Set("num_files", static_cast<int>(sliced.items.size()));
    payload.Set("filenames", std::move(filenames));
    if (sliced.truncated) {
      payload.Set("applied_limit", sliced.applied_limit);
    }
    if (offset > 0) {
      payload.Set("applied_offset", offset);
    }
  }

  std::move(done).Resolve(std::move(payload));
}

}  // namespace nexus

// Definition matching the NEXUS_DECLARE_CATEGORY in the header. Lives
// in the .cc so it can include `tool_session.h` (the header can't —
// `tool_session.h` itself includes process_tool.h to embed
// ProcessTool as a member). The dispatcher in
// nexus_tools_registry.gen.cc invokes this via
// `CategoryAccessor<ProcessTool>::Get(session)`.
NEXUS_DEFINE_CATEGORY(nexus::ProcessTool, process_tool);
