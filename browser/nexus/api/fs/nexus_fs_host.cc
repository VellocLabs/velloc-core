#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

#include <stdint.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/environment.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

// Resolve a guest-supplied path against the host's cwd. Absolute paths
// pass through; relative paths are appended.
base::FilePath ResolvePath(const base::FilePath& cwd,
                           const std::string& path_str) {
  const base::FilePath requested = base::FilePath::FromUTF8Unsafe(path_str);
  if (requested.IsAbsolute()) {
    return requested;
  }
  return cwd.Append(requested);
}

// The stat payload the guest sees, as a dict. Shared by `Stat` (which
// JSON-writes it) and `StatBatch` (which nests it in a row) so the two can
// never drift into reporting different fields for the same file.
// nullopt = the path could not be stat'd.
std::optional<base::DictValue> BuildStatDict(const base::FilePath& resolved) {
  base::File::Info info;
  if (!base::GetFileInfo(resolved, &info)) {
    return std::nullopt;
  }

  base::DictValue dict;
  dict.Set("size", static_cast<double>(info.size));
  dict.Set("isFile", !info.is_directory);
  dict.Set("isDirectory", info.is_directory);
  dict.Set("isSymbolicLink", info.is_symbolic_link);
  dict.Set("isFIFO", false);
  dict.Set("isSocket", false);
  dict.Set("isCharacterDevice", false);
  dict.Set("isBlockDevice", false);

  if (!info.last_modified.is_null()) {
    dict.Set("mtimeMs",
             static_cast<double>(
                 info.last_modified.InMillisecondsFSinceUnixEpoch()));
  }
  if (!info.creation_time.is_null()) {
    const double creation_time =
        static_cast<double>(info.creation_time.InMillisecondsFSinceUnixEpoch());
    dict.Set("ctimeMs", creation_time);
    dict.Set("birthtimeMs", creation_time);
  }
  return dict;
}

// BigBuffer is move-only; the helpers below consume it once.
std::vector<uint8_t> CopyBigBufferBytes(const mojo_base::BigBuffer& buffer) {
  std::vector<uint8_t> bytes(buffer.size());
  base::span(bytes).copy_from(base::span(buffer));
  return bytes;
}

// Reduce a shell path to its interpreter name only — `/usr/bin/zsh` →
// `zsh`, `C:\Program Files\Git\bin\bash.exe` → `bash`. Empty input
// (env var unset or value empty) produces an empty output, which the
// caller treats as "no shell info" and the guest's prompt drops the
// `Shell:` line entirely. Used for both $SHELL and $COMSPEC since
// they share the same path → basename → strip-.exe normalization.
std::string ExtractShellBasename(std::optional<std::string> raw) {
  if (!raw.has_value() || raw->empty()) {
    return std::string();
  }
  base::FilePath shell_path = base::FilePath::FromUTF8Unsafe(*raw);
  std::string basename = shell_path.BaseName().AsUTF8Unsafe();
#if BUILDFLAG(IS_WIN)
  // Strip a trailing .exe on Windows so the model sees "bash" not
  // "bash.exe" — matches the shell names Claude Code uses.
  constexpr char kExeSuffix[] = ".exe";
  constexpr size_t kExeSuffixLen = sizeof(kExeSuffix) - 1;
  if (basename.size() > kExeSuffixLen) {
    const size_t suffix_pos = basename.size() - kExeSuffixLen;
    if (basename.compare(suffix_pos, std::string::npos, kExeSuffix) == 0) {
      basename.resize(suffix_pos);
    }
  }
#endif
  return basename;
}

}  // namespace

// ---- Out-of-line ctor/dtor for move-only Result structs --------------------
//
// chromium-style flags any class with non-trivial members (BigBuffer's
// std::optional, std::string) and inline ctor/dtor; satisfy it with
// `= default;` definitions out-of-line.

NexusFsHost::ReadResult::ReadResult() = default;
NexusFsHost::ReadResult::ReadResult(ReadResult&&) noexcept = default;
NexusFsHost::ReadResult& NexusFsHost::ReadResult::operator=(ReadResult&&)
    noexcept = default;
NexusFsHost::ReadResult::~ReadResult() = default;

NexusFsHost::ReadRangeResult::ReadRangeResult() = default;
NexusFsHost::ReadRangeResult::ReadRangeResult(ReadRangeResult&&) noexcept =
    default;
NexusFsHost::ReadRangeResult&
NexusFsHost::ReadRangeResult::operator=(ReadRangeResult&&) noexcept = default;
NexusFsHost::ReadRangeResult::~ReadRangeResult() = default;

NexusFsHost::StatBatchResult::StatBatchResult() = default;
NexusFsHost::StatBatchResult::StatBatchResult(StatBatchResult&&) noexcept =
    default;
NexusFsHost::StatBatchResult& NexusFsHost::StatBatchResult::operator=(
    StatBatchResult&&) noexcept = default;
NexusFsHost::StatBatchResult::~StatBatchResult() = default;

NexusFsHost::ReaddirTreeResult::ReaddirTreeResult() = default;
NexusFsHost::ReaddirTreeResult::ReaddirTreeResult(
    ReaddirTreeResult&&) noexcept = default;
NexusFsHost::ReaddirTreeResult& NexusFsHost::ReaddirTreeResult::operator=(
    ReaddirTreeResult&&) noexcept = default;
NexusFsHost::ReaddirTreeResult::~ReaddirTreeResult() = default;

NexusFsHost::ReadBatchResult::ReadBatchResult() = default;
NexusFsHost::ReadBatchResult::ReadBatchResult(ReadBatchResult&&) noexcept =
    default;
NexusFsHost::ReadBatchResult& NexusFsHost::ReadBatchResult::operator=(
    ReadBatchResult&&) noexcept = default;
NexusFsHost::ReadBatchResult::~ReadBatchResult() = default;

// ---------------------------------------------------------------------------

NexusFsHost::NexusFsHost(const base::FilePath& cwd) : cwd_(cwd) {
  // One-shot startup: ensure the cwd exists. Posted to thread_pool
  // because the ctor runs on UI and CreateDirectory is blocking. This
  // is the ONLY PostTask in NexusFsHost; per-call work is synchronous.
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](const base::FilePath& path) { base::CreateDirectory(path); },
          cwd_));
}

NexusFsHost::~NexusFsHost() = default;

std::string NexusFsHost::Cwd() const {
  return cwd_.AsUTF8Unsafe();
}

std::string NexusFsHost::UserHome() const {
  // Resolve via env vars first — matches the convention nexus_web uses
  // (`runtimeConfig.getEnv('HOME') ?? runtimeConfig.getEnv('USERPROFILE')`)
  // and, critically, respects overrides set by the e2e test harness
  // (the test browser process is spawned with HOME/USERPROFILE
  // pointing at the per-test isolation root so feature toggles don't
  // pollute the developer's real home dir).
  //
  // On Windows, `base::PathService::Get(base::DIR_HOME)` resolves via
  // `SHGetKnownFolderPath(FOLDERID_Profile)` which IGNORES env vars —
  // so we couldn't use it for the test-isolation case. Reading env
  // directly fixes that.
  //
  // PathService stays as the last-ditch fallback for the (rare)
  // service-account configs where neither env var is set.
  auto env = base::Environment::Create();
#if BUILDFLAG(IS_WIN)
  std::optional<std::string> profile = env->GetVar("USERPROFILE");
  if (profile.has_value() && !profile->empty()) {
    return *profile;
  }
  std::optional<std::string> home_env = env->GetVar("HOME");
  if (home_env.has_value() && !home_env->empty()) {
    return *home_env;
  }
#else
  std::optional<std::string> home_env = env->GetVar("HOME");
  if (home_env.has_value() && !home_env->empty()) {
    return *home_env;
  }
#endif
  base::FilePath home;
  if (base::PathService::Get(base::DIR_HOME, &home)) {
    return home.AsUTF8Unsafe();
  }
  // Empty signal — the bridge maps to its '.' fallback so the WebUI
  // surface keeps working at degraded paths instead of breaking.
  return std::string();
}

NexusFsHost::SystemInfoResult NexusFsHost::SystemInfo() const {
  SystemInfoResult result;

#if BUILDFLAG(IS_WIN)
  result.platform = "win32";
#elif BUILDFLAG(IS_MAC)
  result.platform = "darwin";
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  result.platform = "linux";
#else
  // Unknown platform — leave the field empty so the guest's
  // `# Environment` block drops the `Platform:` line entirely rather
  // than advertising a platform string the model doesn't recognize.
#endif

  // Human-readable "<OS name> <version>" string. base::SysInfo handles
  // the per-platform formatting: Windows uses GetVersionEx-derived
  // strings (e.g. "Windows 11 Pro 10.0.26200"); POSIX returns
  // uname -sr equivalents.
  const std::string os_name = base::SysInfo::OperatingSystemName();
  const std::string os_version = base::SysInfo::OperatingSystemVersion();
  if (!os_name.empty() && !os_version.empty()) {
    result.os_version = os_name + " " + os_version;
  } else if (!os_version.empty()) {
    result.os_version = os_version;
  } else {
    result.os_version = os_name;
  }

  // Best-effort shell detection. The model uses this to disambiguate
  // POSIX-shell-on-Windows (where it should write `/dev/null` not
  // `NUL`) from native PowerShell. We read $SHELL first because it's
  // the interpreter the user actively chose; $COMSPEC on Windows is
  // the cmd.exe fallback and only meaningful if $SHELL is unset.
  //
  // base::Environment::GetVar returns std::optional<std::string> in
  // current Chromium (the older `bool GetVar(name, string*)` out-param
  // form was removed). Use the optional value directly; treat both
  // "not set" and "empty value" as "no shell info available".
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  result.shell = ExtractShellBasename(env->GetVar("SHELL"));
#if BUILDFLAG(IS_WIN)
  // On Windows the user may not have $SHELL set at all. $COMSPEC is
  // always defined (cmd.exe by default). It's a poor proxy for "what
  // shell will the Bash tool actually invoke," but it's better than
  // leaving the line off entirely on a Windows host that hasn't
  // configured $SHELL.
  if (result.shell.empty()) {
    result.shell = ExtractShellBasename(env->GetVar("COMSPEC"));
  }
#endif

  return result;
}

NexusFsHost::StatResult NexusFsHost::Stat(const std::string& path) const {
  StatResult result;
  std::optional<base::DictValue> dict = BuildStatDict(ResolvePath(cwd_, path));
  if (!dict.has_value()) {
    result.error = "File not found or permission denied";
    return result;
  }

  result.success = true;
  base::JSONWriter::Write(base::Value(std::move(*dict)), &result.json);
  return result;
}

NexusFsHost::StatBatchResult NexusFsHost::StatBatch(
    const std::vector<std::string>& paths) const {
  StatBatchResult result;
  for (const std::string& path : paths) {
    base::DictValue row;
    // Echo the REQUEST string, not the resolved path: callers key their
    // results by what they asked for.
    row.Set("path", path);
    std::optional<base::DictValue> stat = BuildStatDict(ResolvePath(cwd_, path));
    if (stat.has_value()) {
      row.Set("stat", base::Value(std::move(*stat)));
    } else {
      // Absent/unreadable is an ordinary answer here, not an error — the
      // callers are existence checks.
      row.Set("stat", base::Value());
    }
    result.rows.Append(std::move(row));
  }
  result.success = true;
  return result;
}

NexusFsHost::ReaddirResult NexusFsHost::Readdir(const std::string& path) const {
  ReaddirResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  if (!base::DirectoryExists(resolved)) {
    result.error = "Path is not a directory";
    return result;
  }

  base::ListValue list;
  base::FileEnumerator enumerator(
      resolved, false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  for (base::FilePath name = enumerator.Next(); !name.empty();
       name = enumerator.Next()) {
    const base::FileEnumerator::FileInfo info = enumerator.GetInfo();
    base::DictValue entry;
    entry.Set("name", name.BaseName().AsUTF8Unsafe());
    entry.Set("isFile", !info.IsDirectory());
    entry.Set("isDirectory", info.IsDirectory());
    entry.Set("isSymbolicLink", false);
    list.Append(std::move(entry));
  }

  result.success = true;
  base::JSONWriter::Write(base::Value(std::move(list)), &result.json);
  return result;
}

NexusFsHost::ReaddirTreeResult NexusFsHost::ReaddirTree(
    const std::string& path,
    int32_t max_depth,
    int32_t max_entries) const {
  ReaddirTreeResult result;
  const base::FilePath root = ResolvePath(cwd_, path);
  result.root = root.AsUTF8Unsafe();

  if (!base::DirectoryExists(root)) {
    result.error = "Path is not a directory";
    return result;
  }

  // Breadth-first with an explicit queue rather than
  // FileEnumerator(recursive=true): recursion would descend into every
  // per-session tool-details/ directory, which on a real profile is tens of
  // thousands of files nobody asked for. BFS (not DFS) also means a
  // truncated scan loses the deepest rows rather than whole subtrees.
  struct Pending {
    base::FilePath dir;
    std::string prefix;  // root-relative, '/'-separated, "" at the root
    int32_t depth;       // levels below the root; the root's children are 1
  };
  std::vector<Pending> queue;
  queue.push_back({root, std::string(), 1});

  for (size_t head = 0; head < queue.size(); ++head) {
    // Copy the fields we need: `queue` reallocates as we push children.
    const base::FilePath dir = queue[head].dir;
    const std::string prefix = queue[head].prefix;
    const int32_t depth = queue[head].depth;

    base::FileEnumerator enumerator(
        dir, false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    for (base::FilePath name = enumerator.Next(); !name.empty();
         name = enumerator.Next()) {
      if (static_cast<int32_t>(result.entries.size()) >= max_entries) {
        result.truncated = true;
        result.success = true;
        return result;
      }

      const base::FileEnumerator::FileInfo info = enumerator.GetInfo();
      const bool is_directory = info.IsDirectory();
      // '/' is a WIRE CONTRACT — the guest splits row paths on it and
      // rebuilds absolutes with its own path joiner. Never emit the
      // platform separator here.
      const std::string relative =
          prefix.empty() ? name.BaseName().AsUTF8Unsafe()
                         : prefix + "/" + name.BaseName().AsUTF8Unsafe();

      base::DictValue entry;
      entry.Set("path", relative);
      entry.Set("isDirectory", is_directory);
      entry.Set("size", is_directory ? 0.0 : static_cast<double>(info.GetSize()));
      const base::Time modified = info.GetLastModifiedTime();
      if (!modified.is_null()) {
        entry.Set("mtimeMs", static_cast<double>(
                                 modified.InMillisecondsFSinceUnixEpoch()));
      }
      result.entries.Append(std::move(entry));

      // Listed at the cap, but not descended.
      if (is_directory && depth < max_depth) {
        queue.push_back({name, relative, depth + 1});
      }
    }
  }

  result.success = true;
  return result;
}

NexusFsHost::ReadBatchResult NexusFsHost::ReadBatch(
    const std::vector<ReadBatchItem>& items,
    size_t max_total_bytes,
    size_t max_item_bytes) const {
  ReadBatchResult result;
  std::vector<uint8_t> blob;

  for (const ReadBatchItem& item : items) {
    base::DictValue entry;
    entry.Set("path", item.path);

    const size_t remaining =
        max_total_bytes > blob.size() ? max_total_bytes - blob.size() : 0;
    if (remaining == 0) {
      result.truncated = true;
      entry.Set("ok", false);
      entry.Set("byteOffset", 0);
      entry.Set("byteLength", 0);
      entry.Set("error", "Batch byte budget exhausted");
      result.entries.Append(std::move(entry));
      continue;
    }

    // Delegate to the single-path readers rather than reimplementing them:
    // that keeps the ENOENT-vs-unreadable wording byte-identical to
    // fs.readFile (the guest regexes for ENOENT), and keeps the 256 MiB
    // range guard in one place. The cost is one extra memcpy per item.
    std::optional<mojo_base::BigBuffer> data;
    std::string error;
    if (item.offset == 0 && item.length == 0) {
      ReadResult read = ReadFile(item.path);
      data = std::move(read.data);
      error = std::move(read.error);
    } else {
      const uint32_t capped = item.length == 0
                                  ? static_cast<uint32_t>(std::min(
                                        max_item_bytes,
                                        static_cast<size_t>(UINT32_MAX)))
                                  : item.length;
      ReadRangeResult read = ReadRange(item.path, item.offset, capped);
      data = std::move(read.data);
      error = std::move(read.error);
    }

    if (!data.has_value()) {
      entry.Set("ok", false);
      entry.Set("byteOffset", 0);
      entry.Set("byteLength", 0);
      entry.Set("error", error.empty() ? "Failed to read file" : error);
      result.entries.Append(std::move(entry));
      continue;
    }

    if (data->size() > max_item_bytes) {
      entry.Set("ok", false);
      entry.Set("byteOffset", 0);
      entry.Set("byteLength", 0);
      entry.Set("error", "Item exceeds the per-item byte limit");
      result.entries.Append(std::move(entry));
      continue;
    }

    if (data->size() > remaining) {
      result.truncated = true;
      entry.Set("ok", false);
      entry.Set("byteOffset", 0);
      entry.Set("byteLength", 0);
      entry.Set("error", "Batch byte budget exhausted");
      result.entries.Append(std::move(entry));
      continue;
    }

    const size_t offset = blob.size();
    const std::vector<uint8_t> bytes = CopyBigBufferBytes(*data);
    blob.insert(blob.end(), bytes.begin(), bytes.end());

    entry.Set("ok", true);
    entry.Set("byteOffset", static_cast<double>(offset));
    entry.Set("byteLength", static_cast<double>(bytes.size()));
    result.entries.Append(std::move(entry));
  }

  result.success = true;
  result.data.emplace(base::span(blob));
  return result;
}

NexusFsHost::ReadResult NexusFsHost::ReadFile(const std::string& path) const {
  ReadResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(resolved);
  if (!bytes.has_value()) {
    // Distinguish "file absent" from "present but unreadable".
    // base::ReadFileToBytes collapses both into nullopt, but JS callers
    // (settings / plugins loaders) treat a missing file as the benign
    // first-run case and only warn on real read errors. Emit ENOENT-style
    // wording for the absent case so the guest's isMissingFileError()
    // detector (regex on ENOENT / "no such file") recognizes it — Node
    // parity, since CC relies on err.code === 'ENOENT'.
    result.error = base::PathExists(resolved)
                       ? "Failed to read file"
                       : "ENOENT: no such file or directory";
    return result;
  }

  result.success = true;
  result.data.emplace(base::span(*bytes));
  return result;
}

NexusFsHost::ReadRangeResult NexusFsHost::ReadRange(const std::string& path,
                                                   uint64_t offset,
                                                   uint32_t length) const {
  ReadRangeResult result;

  // `length` is guest-supplied — cap the allocation before touching it, and
  // never allocate more than the file could actually satisfy from `offset`.
  // Without this a hostile/buggy guest requests up to 4 GiB and the browser
  // process eagerly allocates it before the read.
  constexpr uint32_t kMaxReadRangeBytes = 256 * 1024 * 1024;
  if (length > kMaxReadRangeBytes) {
    result.error = "Requested range is too large";
    return result;
  }

  const base::FilePath resolved = ResolvePath(cwd_, path);

  base::File file(resolved, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    result.error = "Failed to open file";
    return result;
  }

  const int64_t file_length = file.GetLength();
  if (file_length >= 0 && offset < static_cast<uint64_t>(file_length)) {
    const uint64_t available = static_cast<uint64_t>(file_length) - offset;
    if (available < length) {
      length = static_cast<uint32_t>(available);
    }
  } else if (file_length >= 0) {
    length = 0;  // Offset at/past EOF — nothing to read.
  }

  std::vector<uint8_t> buffer(length);
  const std::optional<size_t> read_bytes =
      file.Read(static_cast<int64_t>(offset), base::span(buffer));
  if (!read_bytes.has_value()) {
    result.error = "Failed to read range";
    return result;
  }

  buffer.resize(*read_bytes);
  result.success = true;
  result.data.emplace(base::span(buffer));
  result.bytes_read = static_cast<uint32_t>(*read_bytes);
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::WriteFile(
    const std::string& path,
    mojo_base::BigBuffer content,
    bool atomic) const {
  SimpleResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);
  const std::vector<uint8_t> bytes = CopyBigBufferBytes(content);

  bool written = false;
  if (atomic) {
    // Temp file in the target's directory, then ReplaceFile — a crash at any
    // point leaves either the old file or the new one, never a torn one. The
    // guest asks for this on the session `meta.json`, whose unlogged fields
    // have no other copy.
    written = base::ImportantFileWriter::WriteFileAtomically(
        resolved, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                   bytes.size()));
  } else {
    written = base::WriteFile(resolved, base::span(bytes));
  }
  if (!written) {
    result.error = "Failed to write file";
    return result;
  }
  result.success = true;
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::AppendFile(
    const std::string& path,
    mojo_base::BigBuffer data) const {
  SimpleResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  base::File file(resolved,
                  base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
  if (!file.IsValid()) {
    result.error = "Failed to open file for append";
    return result;
  }
  std::vector<uint8_t> bytes = CopyBigBufferBytes(data);
  if (!file.WriteAtCurrentPosAndCheck(base::span(bytes))) {
    result.error = "Failed to append file";
    return result;
  }
  result.success = true;
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::Mkdir(const std::string& path) const {
  SimpleResult result;
  if (!base::CreateDirectory(ResolvePath(cwd_, path))) {
    result.error = "Failed to create directory";
    return result;
  }
  result.success = true;
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::Rm(const std::string& path,
                                          bool recursive,
                                          bool force) const {
  SimpleResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  if (!base::PathExists(resolved)) {
    result.success = force;
    if (!force) {
      result.error = "Path does not exist";
    }
    return result;
  }

  const bool ok = recursive ? base::DeletePathRecursively(resolved)
                            : base::DeleteFile(resolved);
  if (!ok) {
    result.error = recursive ? "Failed to remove path recursively"
                             : "Failed to remove path";
    return result;
  }

  result.success = true;
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::Unlink(const std::string& path) const {
  SimpleResult result;
  if (!base::DeleteFile(ResolvePath(cwd_, path))) {
    result.error = "Failed to unlink path";
    return result;
  }
  result.success = true;
  return result;
}

NexusFsHost::ExistsResult NexusFsHost::Exists(const std::string& path) const {
  ExistsResult result;
  result.success = true;
  result.exists = base::PathExists(ResolvePath(cwd_, path));
  return result;
}

NexusFsHost::RealpathResult NexusFsHost::Realpath(
    const std::string& path) const {
  RealpathResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  // base::MakeAbsoluteFilePath returns an empty FilePath on failure
  // (target missing, permission denied, broken symlink loop). It
  // resolves symlinks on POSIX and normalizes the path on Windows, which
  // is exactly what JS-side dedup wants: two callers that ended up at
  // the same on-disk file produce the same string.
  base::FilePath canonical = base::MakeAbsoluteFilePath(resolved);
  if (canonical.empty()) {
    result.error = "Failed to resolve canonical path";
    return result;
  }

  result.success = true;
  result.path = canonical.AsUTF8Unsafe();
  return result;
}

}  // namespace nexus
