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
  OnStop("destructor");
}

void FileTool::OnStop(const std::string& reason) {
  LOG(INFO) << "[FileTool] OnStop session=" << GetSessionKey()
            << " reason=" << reason;
  // Invalidating WeakPtrs is the entire stop story for this class —
  // we hold no actor task, no timers, no caches. Replies in flight on
  // ThreadPool will fire harmlessly into a no-op closure.
  weak_ptr_factory_.InvalidateWeakPtrs();
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

  base::DictValue payload;
  payload.Set("file_path", path.AsUTF8Unsafe());
  payload.Set("content", projection.projection);
  payload.Set("start_line", start_line);
  payload.Set("lines_emitted", projection.lines_emitted);
  payload.Set("truncated", projection.truncated);
  // total_bytes is informational — lets the LLM gauge how much it has
  // left to read when chasing a target via offset/limit.
  payload.Set("total_bytes", static_cast<int>(contents.size()));

  result.ok = true;
  result.payload = std::move(payload);
  return result;
}

// static
FileTool::WriteResult FileTool::WriteFileBlocking(base::FilePath path,
                                                  std::string content) {
  WriteResult result;

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

  base::DictValue payload;
  payload.Set("file_path", path.AsUTF8Unsafe());
  payload.Set("bytes_written", static_cast<int>(content.size()));
  result.ok = true;
  result.payload = std::move(payload);
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
  int replacements = 0;
  if (replace_all) {
    updated = contents;
    base::ReplaceSubstringsAfterOffset(&updated, /*start_offset=*/0,
                                       old_string, new_string);
    replacements = static_cast<int>(occurrences);
  } else {
    // Single-occurrence path. We already checked occurrences == 1.
    size_t pos = contents.find(old_string);
    DCHECK(pos != std::string::npos);
    updated.reserve(contents.size() - old_string.size() + new_string.size());
    updated.append(contents, 0, pos);
    updated.append(new_string);
    updated.append(contents, pos + old_string.size(),
                   contents.size() - pos - old_string.size());
    replacements = 1;
  }

  if (!base::WriteFile(path, updated)) {
    result.error_code = "write_failed";
    result.error_message = base::StrCat(
        {"failed to write back after edit: ", path.AsUTF8Unsafe()});
    return result;
  }

  base::DictValue payload;
  payload.Set("file_path", path.AsUTF8Unsafe());
  payload.Set("replacements", replacements);
  payload.Set("bytes_after", static_cast<int>(updated.size()));
  result.ok = true;
  result.payload = std::move(payload);
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

  base::ListValue filenames;
  for (const Match& m : matches) {
    filenames.Append(m.absolute.AsUTF8Unsafe());
  }

  base::TimeDelta duration = base::TimeTicks::Now() - started;

  base::DictValue payload;
  payload.Set("filenames", std::move(filenames));
  payload.Set("num_files", static_cast<int>(matches.size()));
  payload.Set("truncated", truncated);
  payload.Set("duration_ms", static_cast<int>(duration.InMilliseconds()));
  result.ok = true;
  result.payload = std::move(payload);
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
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnWriteDone(ToolCallback done, WriteResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                            std::move(result.error_message));
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
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnGlobDone(ToolCallback done, GlobResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                            std::move(result.error_message));
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
