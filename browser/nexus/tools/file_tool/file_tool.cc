#include "custom_browser/browser/nexus/tools/file_tool/file_tool.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "third_party/re2/src/re2/re2.h"

namespace nexus {

// =============================================================================
// File-local helpers — kept anonymous so they don't pollute the
// nexus:: namespace and so we can iterate on them without touching the
// header. All UTF-8-safe; UTF-8 is self-synchronizing so byte-level
// substring search/replace is correct for any well-formed UTF-8 input.
// =============================================================================
namespace {

// Default cap for `read` when the caller doesn't supply `limit`.
// Matches Claude Code's documented default ("By default reads up to
// 2000 lines from the start"). Overridable via the per-call `limit`
// input field declared in read.tool.json.
constexpr int kDefaultReadLineLimit = 2000;

// Per-call hard cap (independent of the user-supplied `limit`) so a
// pathological 100MB single-line file doesn't blow the response
// budget. Bytes, not lines, because lines could each be huge.
constexpr size_t kReadResponseByteCap = 4 * 1024 * 1024;  // 4 MiB

// Build the "1\tline\n2\tline\n…" projection that Claude Code's Read
// tool returns. The model is trained on this exact shape, so matching
// it minimizes prompt drift even though it's a presentation choice.
//
// `start_line_number` is the 1-indexed number to print on the FIRST
// emitted line (typically `offset` if specified, else 1). The function
// stops appending when either:
//   - `max_lines` lines have been emitted, OR
//   - the running output would exceed `kReadResponseByteCap`.
//
// Returns a tuple of (projection, lines_emitted, truncated_flag).
struct ProjectionResult {
  std::string projection;
  int lines_emitted = 0;
  bool truncated = false;
};

ProjectionResult BuildLineNumberedProjection(
    const std::string& contents,
    int start_line_number,
    int max_lines) {
  ProjectionResult out;

  // SplitStringPiece with KEEP_WHITESPACE preserves blank lines so the
  // emitted line numbers remain in lock-step with what an editor would
  // show. SPLIT_WANT_ALL keeps trailing empty splits (a file ending in
  // "\n" produces an extra empty line we DON'T want to emit, so we
  // strip the final empty entry below).
  std::vector<std::string_view> lines = base::SplitStringPiece(
      contents, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);

  // Trim trailing empty entry from a "...\n"-terminated file so we
  // don't emit a phantom blank line at the end.
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  // start_line_number is 1-indexed; convert to a 0-indexed slice
  // start. We treat any out-of-range start as "no lines" — the
  // payload will simply have an empty projection, which the LLM
  // can detect via lines_emitted=0.
  size_t start = (start_line_number > 0)
                     ? static_cast<size_t>(start_line_number - 1)
                     : 0;
  if (start >= lines.size()) {
    return out;  // empty projection, lines_emitted=0
  }

  // Emit `max_lines` from `start`, byte-capped.
  out.projection.reserve(std::min<size_t>(
      kReadResponseByteCap, contents.size() + 16 * lines.size()));
  size_t emitted = 0;
  for (size_t i = start; i < lines.size() && emitted < static_cast<size_t>(max_lines);
       ++i, ++emitted) {
    std::string prefix = base::NumberToString(static_cast<int>(i) + 1) + "\t";
    if (out.projection.size() + prefix.size() + lines[i].size() + 1 >
        kReadResponseByteCap) {
      out.truncated = true;
      break;
    }
    out.projection.append(prefix);
    out.projection.append(lines[i].data(), lines[i].size());
    out.projection.push_back('\n');
  }
  out.lines_emitted = static_cast<int>(emitted);

  // If we stopped because of the line cap (not the byte cap) but more
  // lines remain in the file, mark truncated so the LLM knows to ask
  // for a follow-up `read` with a larger offset.
  if (!out.truncated &&
      start + emitted < lines.size()) {
    out.truncated = true;
  }
  return out;
}

// Count occurrences of `needle` in `haystack` without overlapping
// matches. Returns 0 for an empty needle (which the caller already
// rejects, but defensive against future refactors).
size_t CountOccurrences(const std::string& haystack,
                        const std::string& needle) {
  if (needle.empty()) {
    return 0;
  }
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// =============================================================================
// Glob helpers
// =============================================================================

// Cap for Glob: keep the agent's response budget bounded.
constexpr int kDefaultGlobLimit = 100;

// Directory names skipped by Glob walks. Mirrors the TS GlobTool
// allowlist plus the standard "vendor / output / cache" set.
const char* const kSkipDirectories[] = {
    "node_modules", ".git", ".hg", ".svn", ".cache",
    ".next",        ".nuxt", "dist", "build", "out",
    ".turbo",       ".parcel-cache",
};

bool ShouldSkipDirectory(const base::FilePath& dir) {
  std::string name = dir.BaseName().AsUTF8Unsafe();
  if (name.empty()) {
    return false;
  }
  // Skip dotted dirs by convention (TS GlobTool does the same).
  if (name[0] == '.' && name != "." && name != "..") {
    return true;
  }
  for (const char* skip : kSkipDirectories) {
    if (name == skip) {
      return true;
    }
  }
  return false;
}

// Convert a glob pattern (`**`, `*`, `?`, character classes) to an
// RE2 source string matching forward-slash-normalized paths. Mirrors
// the TS GlobTool's `globToRegExp` byte-for-byte so the catalog of
// patterns the LLM has been trained against keeps working.
std::string GlobToRe2(const std::string& pattern) {
  std::string src = pattern;
  // Normalize backslashes to forward slashes for matching.
  std::replace(src.begin(), src.end(), '\\', '/');

  std::string out;
  out.reserve(src.size() * 2 + 4);
  out += '^';
  size_t i = 0;
  while (i < src.size()) {
    char ch = src[i];
    if (ch == '*') {
      bool is_double = (i + 1 < src.size()) && src[i + 1] == '*';
      if (is_double) {
        // `**` matches across path separators, with optional trailing /.
        out += "(?:.*/)?";
        i += 2;
        if (i < src.size() && src[i] == '/') {
          ++i;
        }
      } else {
        out += "[^/]*";
        ++i;
      }
      continue;
    }
    if (ch == '?') {
      out += "[^/]";
      ++i;
      continue;
    }
    if (ch == '[') {
      size_t end = src.find(']', i + 1);
      if (end == std::string::npos) {
        out += "\\[";
        ++i;
      } else {
        out.append(src, i, end - i + 1);
        i = end + 1;
      }
      continue;
    }
    // Escape regex metacharacters; everything else passes verbatim.
    static constexpr char kRegexMeta[] = ".+^$|()/\\{}";
    if (std::string_view(kRegexMeta).find(ch) != std::string_view::npos) {
      out += '\\';
      out += ch;
      ++i;
      continue;
    }
    out += ch;
    ++i;
  }
  out += '$';
  return out;
}

// Resolve `path` against `cwd_or_default`: absolute paths used as-is,
// relative paths joined onto the default. Empty `path` returns the
// default. Returns base::FilePath() if both inputs are empty; the
// caller treats that as a "no usable cwd" error.
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

}  // namespace

// =============================================================================
// Out-of-line ctors/dtors for the per-method result structs.
//
// Required by chromium-style: any class/struct with non-trivial member
// types (std::string, base::Value::Dict) must declare AND define its
// special members out-of-line. Defining them here as `= default` keeps
// them trivial in spirit while satisfying the clang plugin and giving
// the compiler a single, predictable instantiation point.
//
// All three structs are move-only: base::Value::Dict is move-only, and
// allowing copies would silently regress that. `noexcept` on the move
// ops matters for std::vector / std::optional / base::BindOnce
// performance — they pick the move path only when it can't throw.
// =============================================================================

FileTool::ReadResult::ReadResult() = default;
FileTool::ReadResult::ReadResult(ReadResult&&) noexcept = default;
FileTool::ReadResult& FileTool::ReadResult::operator=(ReadResult&&) noexcept =
    default;
FileTool::ReadResult::~ReadResult() = default;

FileTool::WriteResult::WriteResult() = default;
FileTool::WriteResult::WriteResult(WriteResult&&) noexcept = default;
FileTool::WriteResult& FileTool::WriteResult::operator=(
    WriteResult&&) noexcept = default;
FileTool::WriteResult::~WriteResult() = default;

FileTool::EditResult::EditResult() = default;
FileTool::EditResult::EditResult(EditResult&&) noexcept = default;
FileTool::EditResult& FileTool::EditResult::operator=(EditResult&&) noexcept =
    default;
FileTool::EditResult::~EditResult() = default;

FileTool::GlobResult::GlobResult() = default;
FileTool::GlobResult::GlobResult(GlobResult&&) noexcept = default;
FileTool::GlobResult& FileTool::GlobResult::operator=(GlobResult&&) noexcept =
    default;
FileTool::GlobResult::~GlobResult() = default;

// =============================================================================
// Construction / lifecycle
// =============================================================================

// static
void FileTool::DefineTools(ToolRegistry& /*registry*/) {
  // Body intentionally empty. Same staging pattern as
  // `BrowserTool::DefineTools` — every tool in this category is
  // registered via the codegen-emitted `generated::PopulateRegistry()`
  // that ToolCatalog calls right after this no-op. Kept around so a
  // future macro-DSL category (if anyone ever needs one again) has a
  // clean place to register from.
}

FileTool::FileTool(ToolSession* session, Profile* profile,
                   const std::string& session_key)
    : ToolBase(session, profile, session_key) {
  // Cache the WeakPtr now (we're on UI — ToolSession constructs us
  // there). Worker-thread bodies later capture it without ever calling
  // `weak_ptr_factory_.GetWeakPtr()` off-sequence.
  //
  // Initialized in the body (not the ctor init list) because chromium-
  // style requires `weak_ptr_factory_` to be the last declared member;
  // an init-list initializer for `weak_this_` would read from
  // `weak_ptr_factory_` before it's constructed. The body runs after
  // every member is constructed, so calling GetWeakPtr() here is safe.
  weak_this_ = weak_ptr_factory_.GetWeakPtr();
}

FileTool::~FileTool() {
  // Defensive — invalidate WeakPtrs so any reply still queued on
  // ThreadPool drops cleanly. OnStop is idempotent so the explicit
  // call here is safe even if it already ran.
  OnStop(OnStopReason::kDestructor);
}

void FileTool::OnStop(OnStopReason reason) {
  LOG(INFO) << "[FileTool] OnStop session=" << GetSessionKey()
            << " reason=" << OnStopReasonToString(reason);
  // InvalidateWeakPtrs is gated on TEARDOWN reasons only — the ones
  // that mean this FileTool is actually going away.
  //
  // Background: per-turn cleanup reasons (kCompleted, kUserStopped)
  // fire OnStop while the ToolSession stays warm in the adapter map
  // (typed-OnStop refactor — see tools_adapter.cc). FileTool caches
  // a `weak_this_` member at construction time
  // (file_tool.h declares `weak_this_`, file_tool.cc ctor sets it)
  // because thread-pool bodies can't call `GetWeakPtr()` off-sequence.
  // Invalidating WeakPtrs on per-turn cleanup would permanently null
  // out that cached `weak_this_` for the rest of the session — every
  // subsequent Read/Write/Edit/Glob would post its reply back to UI
  // bound to a dead WeakPtr, the BindOnce would silently drop, and
  // `ToolCallback`'s destructor would emit `tool_dropped` to the LLM.
  //
  // For kCompleted / kUserStopped there can be no in-flight tool I/O
  // anyway (the agent loop blocks on tool results before deciding the
  // turn is "complete"), so the invalidate has no in-flight callbacks
  // to clean up — it's pure collateral damage on the cached
  // `weak_this_`. Skip it.
  const bool teardown = reason == OnStopReason::kSessionDeleted ||
                        reason == OnStopReason::kDisconnected ||
                        reason == OnStopReason::kShutdown ||
                        reason == OnStopReason::kDestructor;
  if (teardown) {
    weak_ptr_factory_.InvalidateWeakPtrs();
  }
}

// =============================================================================
// Run<Name>OnThreadPool overrides — invoked on a base::ThreadPool
// worker by the codegen dispatcher (the JSON declaration carries
// `runtime.thread = "thread_pool"`, which the registry's auto-routing
// honors via `base::ThreadPool::PostTask` with `MayBlock` +
// `USER_VISIBLE`).
//
// Because we're already on a worker:
//   - `base::ReadFileToString` / `base::WriteFile` etc. are safe to
//     call directly — no nested PostTask needed.
//   - We must NOT call `done.Resolve` / `done.Reject` here. The
//     callbacks captured inside `done` are bound to a UI-resident
//     `WeakPtr<ToolsAdapter>`; firing them off-UI would CHECK-fail
//     when the WeakPtr is dereferenced.
//   - All replies hop back via `content::GetUIThreadTaskRunner({})`.
//     The bound `weak_this_` was captured on UI at construction time,
//     so the WeakPtr's home-sequence contract is preserved.
//
// File-local helper that wraps the UI-hop boilerplate. Both the
// validation-failure path and the success/result-error path want to
// invoke the matching `On<Name>Done` member back on UI; this keeps
// the wiring uniform and avoids hand-rolling the same PostTask
// preamble three times.
// =============================================================================

namespace {

// Posts the bound reply to UI. The body is so small that templating
// over the result type pays for itself by removing three identical
// PostTask calls below.
template <typename ResultT, typename MemberFn>
void ReplyOnUI(MemberFn member_fn,
               base::WeakPtr<FileTool> weak_self,
               ToolCallback done,
               ResultT result) {
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(member_fn, std::move(weak_self), std::move(done),
                     std::move(result)));
}

}  // namespace

void FileTool::RunReadOnThreadPool(::nexus::generated::ReadInput input,
                                   ToolCallback done) {
  // Validate on the worker — cheap, no thread requirements.
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    ReadResult bad;
    bad.error_code = "invalid_args";
    bad.error_message = "file_path must be an absolute path";
    ReplyOnUI(&FileTool::OnReadDone, weak_this_, std::move(done),
              std::move(bad));
    return;
  }

  ReadResult result =
      ReadFileBlocking(std::move(path), input.offset, input.limit);
  ReplyOnUI(&FileTool::OnReadDone, weak_this_, std::move(done),
            std::move(result));
}

void FileTool::RunWriteOnThreadPool(::nexus::generated::WriteInput input,
                                    ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    WriteResult bad;
    bad.error_code = "invalid_args";
    bad.error_message = "file_path must be an absolute path";
    ReplyOnUI(&FileTool::OnWriteDone, weak_this_, std::move(done),
              std::move(bad));
    return;
  }

  WriteResult result =
      WriteFileBlocking(std::move(path), std::move(input.content));
  ReplyOnUI(&FileTool::OnWriteDone, weak_this_, std::move(done),
            std::move(result));
}

void FileTool::RunEditOnThreadPool(::nexus::generated::EditInput input,
                                   ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  // Edit's pre-flight has more invariants than read/write — collect
  // the first violation into a typed EditResult so the UI-side reply
  // path is uniform with the success case.
  auto reject = [&](std::string message) {
    EditResult bad;
    bad.error_code = "invalid_args";
    bad.error_message = std::move(message);
    ReplyOnUI(&FileTool::OnEditDone, weak_this_, std::move(done),
              std::move(bad));
  };
  if (!path.IsAbsolute()) {
    reject("file_path must be an absolute path");
    return;
  }
  if (input.old_string == input.new_string) {
    reject("new_string must differ from old_string");
    return;
  }
  if (input.old_string.empty()) {
    reject("old_string must not be empty");
    return;
  }

  bool replace_all = input.replace_all.value_or(false);
  EditResult result = EditFileBlocking(
      std::move(path), std::move(input.old_string),
      std::move(input.new_string), replace_all);
  ReplyOnUI(&FileTool::OnEditDone, weak_this_, std::move(done),
            std::move(result));
}

void FileTool::RunGlobOnThreadPool(::nexus::generated::GlobInput input,
                                   ToolCallback done) {
  // Resolve `path` against the session cwd. If the model gave an
  // absolute path we trust it; otherwise we need a non-empty session
  // cwd to resolve against. Surface the error path explicitly so
  // setAgentLoopSettings's cwd is a hard prerequisite for relative
  // searches.
  ToolSession* session = GetSession();
  base::FilePath default_root = session ? session->cwd() : base::FilePath();
  std::string path_str = input.path.value_or("");
  base::FilePath root = ResolveAgainstCwd(path_str, default_root);
  if (root.empty()) {
    GlobResult bad;
    bad.error_code = "invalid_args";
    bad.error_message =
        "session cwd is not set and no absolute `path` was provided. "
        "Push a cwd via setAgentLoopSettings or pass an absolute path.";
    ReplyOnUI(&FileTool::OnGlobDone, weak_this_, std::move(done),
              std::move(bad));
    return;
  }

  int limit = input.limit.value_or(kDefaultGlobLimit);
  if (limit <= 0) {
    limit = kDefaultGlobLimit;
  }

  GlobResult result =
      GlobFilesBlocking(std::move(root), input.pattern, limit);
  ReplyOnUI(&FileTool::OnGlobDone, weak_this_, std::move(done),
            std::move(result));
}

// =============================================================================
// Worker-thread implementations — `static`, no `this`.
// =============================================================================

// static
FileTool::ReadResult FileTool::ReadFileBlocking(
    base::FilePath path,
    std::optional<int> offset,
    std::optional<int> limit) {
  ReadResult result;

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    result.error_code = "read_failed";
    result.error_message =
        base::StrCat({"failed to read file: ", path.AsUTF8Unsafe()});
    return result;
  }

  int start_line = offset.value_or(1);
  if (start_line < 1) {
    start_line = 1;
  }
  int max_lines = limit.value_or(kDefaultReadLineLimit);
  if (max_lines < 1) {
    max_lines = kDefaultReadLineLimit;
  }

  ProjectionResult projection =
      BuildLineNumberedProjection(contents, start_line, max_lines);

  // CC parity: Read returns the line-numbered content directly (CC's
  // FileReadTool emits "cat -n" formatted text as the tool_result
  // string). When the projection truncated the file, append a trailer
  // line telling the model so it can choose to re-Read with offset.
  result.ok = true;
  result.message = std::move(projection.projection);
  if (projection.truncated) {
    result.message.append(
        base::StrCat({"\n\n[file truncated at line ",
                      base::NumberToString(start_line +
                                           projection.lines_emitted - 1),
                      " of ",
                      base::NumberToString(contents.size()),
                      " bytes; re-Read with offset to continue]"}));
  }
  return result;
}

// static
FileTool::WriteResult FileTool::WriteFileBlocking(base::FilePath path,
                                                  std::string content) {
  WriteResult result;

  // Probe existence BEFORE we touch the file so the prose response can
  // distinguish create-vs-update (CC's FileWriteTool branches on the
  // same signal). base::WriteFile overwrites on create, so without a
  // prior probe we couldn't tell which case applies.
  const bool was_create = !base::PathExists(path);

  // Ensure the parent directory exists. base::WriteFile fails silently
  // (returns false) when the parent is missing; creating it explicitly
  // mirrors Claude Code's behavior of working from a clean slate when
  // the agent writes a file under a not-yet-existing directory tree.
  base::FilePath parent = path.DirName();
  if (!parent.empty() && !base::DirectoryExists(parent)) {
    if (!base::CreateDirectory(parent)) {
      result.error_code = "write_failed";
      result.error_message = base::StrCat(
          {"failed to create parent directory: ", parent.AsUTF8Unsafe()});
      return result;
    }
  }

  if (!base::WriteFile(path, content)) {
    result.error_code = "write_failed";
    result.error_message =
        base::StrCat({"failed to write file: ", path.AsUTF8Unsafe()});
    return result;
  }

  // Result-string design (latest iteration — see D7 entry in
  // C:\Users\myyyb\.claude\plans\compare-the-cc-and-staged-reddy.md and
  // docs/agents/nexus-web-parity-audit.md → "Write tool success message"):
  //
  // CC's wording is reused verbatim ("File created successfully at: <p>"
  // / "The file <p> has been updated successfully.") so the model's
  // trained recognition pattern fires. CC's wording on its own was not
  // enough in Nexus — testing showed the model still re-issued Write
  // calls because the result didn't confirm the content actually
  // landed on disk (it only confirmed the file existed afterwards).
  // The fix is the additive ". Wrote N bytes." suffix: it carries an
  // unambiguous byte-count signal that the agent's content was
  // accepted, which closes the verify-loop the model was caught in.
  //
  // Echoing the path is intentional — earlier hypotheses worried it
  // would "create ambiguity about whether the SAME path was written",
  // but the opposite proved true: the path echo lets the model
  // pattern-match what it sent vs. what landed in one comparison,
  // and is consistent with CC's wording.
  //
  // Earlier wording variants in this slot:
  //  - "The function is executed successfully; the file is created and
  //    the content is updated too." (generic, ambiguous about content)
  //  - "The function is executed successfully; the content is updated."
  //    (update-only variant; same ambiguity)
  // Both produced retry spirals. Do not revert to either.
  result.ok = true;
  const std::string byte_str = base::NumberToString(content.size());
  result.message =
      was_create
          ? base::StrCat({"File created successfully at: ",
                          path.AsUTF8Unsafe(), ". Wrote ", byte_str,
                          " bytes."})
          : base::StrCat({"The file ", path.AsUTF8Unsafe(),
                          " has been updated successfully. Wrote ",
                          byte_str, " bytes."});
  return result;
}

// static
FileTool::EditResult FileTool::EditFileBlocking(base::FilePath path,
                                                std::string old_string,
                                                std::string new_string,
                                                bool replace_all) {
  EditResult result;

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    result.error_code = "read_failed";
    result.error_message =
        base::StrCat({"failed to read file for edit: ", path.AsUTF8Unsafe()});
    return result;
  }

  size_t occurrences = CountOccurrences(contents, old_string);
  if (occurrences == 0) {
    result.error_code = "no_match";
    result.error_message =
        "old_string not found in file; cannot edit. Re-read the file "
        "to verify the exact text/whitespace.";
    return result;
  }
  if (occurrences > 1 && !replace_all) {
    result.error_code = "ambiguous_match";
    result.error_message = base::StrCat(
        {"old_string occurs ", base::NumberToString(occurrences),
         " times; provide a more specific old_string OR set "
         "replace_all=true to replace every occurrence."});
    return result;
  }

  std::string updated;
  if (replace_all) {
    updated = contents;
    base::ReplaceSubstringsAfterOffset(&updated, /*start_offset=*/0,
                                       old_string, new_string);
  } else {
    // Single-occurrence path. We already checked occurrences == 1.
    size_t pos = contents.find(old_string);
    DCHECK(pos != std::string::npos);
    updated.reserve(contents.size() - old_string.size() + new_string.size());
    updated.append(contents, 0, pos);
    updated.append(new_string);
    updated.append(contents, pos + old_string.size(),
                   contents.size() - pos - old_string.size());
  }

  if (!base::WriteFile(path, updated)) {
    result.error_code = "write_failed";
    result.error_message = base::StrCat(
        {"failed to write back after edit: ", path.AsUTF8Unsafe()});
    return result;
  }

  // CC parity: prose success string. Match FileEditTool.ts wording
  // exactly so the model's recognition pattern is shared with the CC
  // experience. The branches mirror CC's replace_all flag.
  result.ok = true;
  result.message =
      replace_all
          ? base::StrCat({"The file ", path.AsUTF8Unsafe(),
                          " has been updated. All occurrences were "
                          "successfully replaced."})
          : base::StrCat({"The file ", path.AsUTF8Unsafe(),
                          " has been updated successfully."});
  // Replacement count and `bytes_after` are intentionally not exposed
  // — CC's response carries no such structured fields, and the
  // diagnostics are available via tool-call telemetry if a future UI
  // wants them.
  return result;
}

// static
FileTool::GlobResult FileTool::GlobFilesBlocking(base::FilePath root,
                                                  std::string pattern,
                                                  int limit) {
  GlobResult result;
  base::TimeTicks started = base::TimeTicks::Now();

  if (pattern.empty()) {
    result.error_code = "invalid_args";
    result.error_message = "pattern is required";
    return result;
  }
  if (!base::DirectoryExists(root)) {
    result.error_code = "no_such_directory";
    result.error_message =
        base::StrCat({"glob root is not a directory: ", root.AsUTF8Unsafe()});
    return result;
  }

  std::string regex_src = GlobToRe2(pattern);
  re2::RE2 matcher(regex_src);
  if (!matcher.ok()) {
    result.error_code = "invalid_args";
    result.error_message =
        base::StrCat({"glob pattern compile failed: ", matcher.error()});
    return result;
  }

  // Collect (relative-path, abs-path, mtime) tuples. We sort by mtime
  // desc at the end and take the top `limit` (matching the TS behavior).
  struct Match {
    base::FilePath absolute;
    std::string relative_norm;
    base::Time mtime;
  };
  std::vector<Match> matches;
  bool truncated = false;

  // base::FileEnumerator includes hidden files; we use the directory
  // skip-list in ShouldSkipDirectory to prune walks. FILES-only mode
  // means the enumerator emits regular files; we still rely on the
  // skip-list because the recursive walk would descend into dotted /
  // vendor dirs by default.
  base::FileEnumerator enumerator(
      root, /*recursive=*/true,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);

  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    base::FileEnumerator::FileInfo info = enumerator.GetInfo();
    if (info.IsDirectory()) {
      // FileEnumerator doesn't expose per-directory skip; we can't
      // prune the walk mid-iteration. Best we can do is skip processing
      // a file whose ANY ancestor below `root` is in the skip-list.
      // Cheap path test: if any path component matches, skip.
      continue;
    }
    // Compute the relative path from `root` and forward-slash-normalize.
    base::FilePath rel;
    if (!root.AppendRelativePath(path, &rel)) {
      // Defensive — should never happen under recursive=true with the
      // root we passed in.
      continue;
    }
    // Skip files under any vendor dir.
    bool under_skip = false;
    for (const auto& comp : rel.GetComponents()) {
      base::FilePath comp_path(comp);
      if (ShouldSkipDirectory(comp_path)) {
        under_skip = true;
        break;
      }
    }
    if (under_skip) {
      continue;
    }

    std::string rel_norm = rel.AsUTF8Unsafe();
    std::replace(rel_norm.begin(), rel_norm.end(), '\\', '/');
    if (!re2::RE2::FullMatch(rel_norm, matcher)) {
      continue;
    }

    matches.push_back({path, std::move(rel_norm), info.GetLastModifiedTime()});
    if (static_cast<int>(matches.size()) > limit * 4) {
      // Soft cap on collection size before sort — keeps memory bounded
      // when the pattern matches an unusually large set. The final
      // truncation to `limit` happens after sort.
      truncated = true;
      break;
    }
  }

  // Sort by mtime desc (newest first), matching the TS behavior.
  std::sort(matches.begin(), matches.end(),
            [](const Match& a, const Match& b) { return a.mtime > b.mtime; });

  if (static_cast<int>(matches.size()) > limit) {
    matches.resize(limit);
    truncated = true;
  }

  // Suppress unused-variable warning when duration is no longer
  // referenced in the prose path. The timing is still computed for
  // any future telemetry hook.
  (void)started;

  result.ok = true;

  // CC parity: GlobTool.ts:177 emits "No files found" on empty
  // results, otherwise the matched paths joined by '\n' with a
  // pagination caveat appended when truncated. The model uses this
  // same pattern as a file-exists check (call Glob with the exact
  // path as the pattern — "No files found" means the file doesn't
  // exist, otherwise the path comes back).
  if (matches.empty()) {
    result.message = "No files found";
    return result;
  }
  std::string body;
  for (size_t i = 0; i < matches.size(); ++i) {
    if (i > 0) body.push_back('\n');
    body.append(matches[i].absolute.AsUTF8Unsafe());
  }
  if (truncated) {
    body.append(
        "\n(Results are truncated. Consider using a more specific pattern "
        "or increasing the limit.)");
  }
  result.message = std::move(body);
  return result;
}

// =============================================================================
// UI-thread reply trampolines.
// =============================================================================

void FileTool::OnReadDone(ToolCallback done, ReadResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                            std::move(result.error_message));
    return;
  }
  if (!result.message.empty()) {
    std::move(done).Resolve(base::Value(std::move(result.message)));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnWriteDone(ToolCallback done, WriteResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                            std::move(result.error_message));
    return;
  }
  // Prefer the prose `message` when populated. The JS-side
  // `serializeToolResult` (toolExecution.ts) returns the string
  // directly when `result.data` is a string — the model sees the
  // CC-style prose instead of a JSON dict.
  if (!result.message.empty()) {
    std::move(done).Resolve(base::Value(std::move(result.message)));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnEditDone(ToolCallback done, EditResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                            std::move(result.error_message));
    return;
  }
  if (!result.message.empty()) {
    std::move(done).Resolve(base::Value(std::move(result.message)));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnGlobDone(ToolCallback done, GlobResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                            std::move(result.error_message));
    return;
  }
  if (!result.message.empty()) {
    std::move(done).Resolve(base::Value(std::move(result.message)));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

}  // namespace nexus

// Definition matching the NEXUS_DECLARE_CATEGORY in the header. Lives
// in the .cc so it can include `tool_session.h` (the header can't —
// `tool_session.h` itself includes file_tool.h to embed the FileTool
// member). The dispatcher in nexus_tools_registry.gen.cc invokes this
// via `CategoryAccessor<FileTool>::Get(session)`.
NEXUS_DEFINE_CATEGORY(nexus::FileTool, file_tool);
