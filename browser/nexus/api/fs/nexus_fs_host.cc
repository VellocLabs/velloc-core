#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

#include <stdint.h>

#include <algorithm>
#include <cmath>
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
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/api/shell/host_shell.h"
#include "mojo/public/cpp/base/big_buffer.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

// NEXUS_FS_ATOMIC_WRITE is defined in nexus_fs_host.h — see the flag's comment
// there for why it is off and what turning it back on restores.
#if NEXUS_FS_ATOMIC_WRITE
#include "base/files/important_file_writer.h"
#endif

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

// `absolute` with its links resolved (base::NormalizeFilePath), or empty when
// that is not possible. A path that does not exist yet is resolved through
// its deepest existing ancestor, so `<root>/link/new.txt` with `link`
// pointing elsewhere comes back outside `<root>` even before `new.txt` exists.
// A missing tail that climbs (`..`) or names `.` is refused: which directory it
// lands in depends on the links it would cross, so it cannot be appended.
base::FilePath ResolveLinks(const base::FilePath& absolute) {
  base::FilePath existing = absolute.StripTrailingSeparators();
  std::vector<base::FilePath::StringType> missing_tail;
  while (!base::PathExists(existing)) {
    const base::FilePath parent = existing.DirName();
    const base::FilePath::StringType name = existing.BaseName().value();
    if (parent == existing || name == base::FilePath::kCurrentDirectory ||
        name == base::FilePath::kParentDirectory) {
      return base::FilePath();
    }
    missing_tail.push_back(name);
    existing = parent;
  }
  base::FilePath resolved;
  if (!base::NormalizeFilePath(existing, &resolved)) {
    return base::FilePath();
  }
  for (auto it = missing_tail.rbegin(); it != missing_tail.rend(); ++it) {
    resolved = resolved.Append(*it);
  }
  return resolved;
}

#if NEXUS_FS_ATOMIC_WRITE
// Atomic write with a SHORT temp name, for targets too deep for the upstream
// writer. Same shape as `base::ImportantFileWriter` — sibling temp, flush,
// replace — but named `<target>.tmp`.
//
// Why it has to exist: upstream names its temp file with a random 36-char
// GUID plus ".tmp" (`CreateAndOpenTemporaryFileInDir`,
// base/files/file_util_win.cc), so an atomic write needs 41 characters of
// headroom past the target's DIRECTORY, regardless of how short the target's
// own name is. On Windows that fails once `dir + 41 > MAX_PATH` — roughly 32
// characters BEFORE the target path itself would be too long, so a plain write
// to the very same path still succeeds. A background sub-agent's `meta.json`
// sits deep enough to cross that line,
//   projects/<sanitized-project-root>/<parentSessionId>/subagents/<subId>/
// and the failure is not cosmetic: the write is the sub-agent's first, so it
// aborted the whole sub-agent with "Failed to write file".
//
// `<target>.tmp` costs 4 characters instead of 41, which keeps the atomic
// path's reach within a few characters of the plain one. The fixed name is
// safe here where a shared temp name would not be: the guest serializes writes
// per path (`BridgeFsSessionStorage`'s per-session queue), and the temp is
// derived from the target rather than shared across targets.
bool WriteFileAtomicallyShortTemp(const base::FilePath& target,
                                  base::span<const uint8_t> bytes) {
  const base::FilePath tmp = target.AddExtension(FILE_PATH_LITERAL("tmp"));
  {
    base::File file(tmp,
                    base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    if (!file.IsValid()) {
      return false;
    }
    // Flush before the replace: the point of the temp file is that a crash
    // leaves either the old contents or the new, never a half-written file.
    if (!file.WriteAndCheck(0, bytes) || !file.Flush()) {
      file.Close();
      base::DeleteFile(tmp);
      return false;
    }
  }
  if (!base::ReplaceFile(tmp, target, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}
#endif  // NEXUS_FS_ATOMIC_WRITE

// The stat payload the guest sees, as a dict. Shared by `Stat` (which
// JSON-writes it) and `StatBatch` (which nests it in a row) so the two can
// never drift into reporting different fields for the same file.
// nullopt = the path could not be stat'd; when the caller supplies
// `out_error`, the OS failure kind (base::File::GetLastFileError — the
// thread's errno/GetLastError, read before any other filesystem call runs)
// is written there so `Stat` can discriminate its wire message. NOTE:
// GetFileInfo is a free base:: helper, so a ScopedBlockingCall teardown runs
// between the failing syscall and this read — that teardown is
// last-error-safe by construction (~UncheckedScopedBlockingCall holds
// base::ScopedClearLastError), which is what makes this capture sound.
std::optional<base::DictValue> BuildStatDict(const base::FilePath& resolved,
                                             base::File::Error* out_error) {
  base::File::Info info;
  if (!base::GetFileInfo(resolved, &info)) {
    if (out_error) {
      *out_error = base::File::GetLastFileError();
    }
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

}  // namespace

// The one canonical spelling of the absent-path wire code (see
// FsFailureMessage's header comment) — shared so no method can drift into a
// fourth variant.
constexpr char kEnoentMessage[] = "ENOENT: no such file or directory";

std::string FsFailureMessage(base::File::Error error) {
  switch (error) {
    case base::File::FILE_ERROR_NOT_FOUND:
      return kEnoentMessage;
    case base::File::FILE_ERROR_ACCESS_DENIED:
      return "EACCES: permission denied";
    case base::File::FILE_ERROR_IN_USE:
      return "EBUSY: file is in use or locked";
    // A file sitting where a directory component was expected: the path as
    // named definitely does not exist, so the guest treats ENOTDIR like
    // ENOENT ("safely absent") — that keeps getFileHandle(create:true)
    // behavior identical to the pre-taxonomy existence-probe fallback.
    case base::File::FILE_ERROR_NOT_A_DIRECTORY:
      return "ENOTDIR: not a directory";
    case base::File::FILE_ERROR_IO:
      return "EIO: input/output error";
    default:
      // Raw enum VALUE on purpose — never ErrorToString's chromium-versioned
      // tokens (header comment). Reads as transient guest-side, which is the
      // safe default for anything undiagnosed.
      return "EUNKNOWN: filesystem operation failed (error " +
             base::NumberToString(static_cast<int>(error)) + ")";
  }
}

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

  // The shell the model's shell tool runs — `bash` whenever the Bash tool
  // is offered on Windows, not `%COMSPEC%` (host_shell.h). Stats files; this
  // runs on the MayBlock pool (HandleSystemInfoOnUI).
  result.shell = ResolveReportedShellName();

  return result;
}

NexusFsHost::PathsResult::PathsResult() = default;
NexusFsHost::PathsResult::PathsResult(const PathsResult&) = default;
NexusFsHost::PathsResult& NexusFsHost::PathsResult::operator=(
    const PathsResult&) = default;
NexusFsHost::PathsResult::~PathsResult() = default;

NexusFsHost::PathsResult NexusFsHost::Paths() const {
  PathsResult result;
  result.home = UserHome();
  base::FilePath dir;
  if (base::GetTempDir(&dir)) {
    result.temp = dir.AsUTF8Unsafe();
  }
#if BUILDFLAG(IS_WIN)
  if (base::PathService::Get(base::DIR_ROAMING_APP_DATA, &dir)) {
    result.app_data = dir.AsUTF8Unsafe();
  }
  if (base::PathService::Get(base::DIR_LOCAL_APP_DATA, &dir)) {
    result.cache = dir.AsUTF8Unsafe();
  }
#elif BUILDFLAG(IS_MAC)
  if (base::PathService::Get(base::DIR_APP_DATA, &dir)) {
    result.app_data = dir.AsUTF8Unsafe();
  }
  if (base::PathService::Get(base::DIR_CACHE, &dir)) {
    result.cache = dir.AsUTF8Unsafe();
  }
#elif BUILDFLAG(IS_POSIX)
  // XDG base directories, with the spec's defaults under home.
  auto env = base::Environment::Create();
  std::optional<std::string> config_home = env->GetVar("XDG_CONFIG_HOME");
  if (config_home.has_value() && !config_home->empty()) {
    result.app_data = *config_home;
  } else if (!result.home.empty()) {
    result.app_data =
        base::FilePath::FromUTF8Unsafe(result.home).Append(".config").value();
  }
  if (base::PathService::Get(base::DIR_CACHE, &dir)) {
    result.cache = dir.AsUTF8Unsafe();
  }
#endif
  return result;
}

NexusFsHost::StatResult NexusFsHost::Stat(const std::string& path) const {
  StatResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);
  base::File::Error stat_error = base::File::FILE_OK;
  std::optional<base::DictValue> dict = BuildStatDict(resolved, &stat_error);
  if (!dict.has_value()) {
    // Discriminated, Node-style wording — a WIRE CONTRACT like ReadFile's
    // ENOENT line and Rename's strings: the guest keys on the leading code
    // to tell "genuinely absent" (safe to treat as no file) from transient
    // failures (permission, lock, I/O) that must NOT read as absent —
    // SuperEdit's commit conflict check is the first consumer (see
    // nexus_web utils/fsTransportFailure.ts + the internal repo's
    // docs/tools/nexus-run-js.md). The old single wording ("File not found
    // or permission denied") is retired; an older guest treats any of these
    // as absent, which is exactly its previous behavior — degradable, no
    // interface bump.
    //
    // Deliberately NO existence-probe fallback for unmapped errors: a
    // base::PathExists on a path whose stat just failed with EIO / a
    // network blip fails for the SAME underlying reason and would answer
    // "absent" — re-opening the transient-reads-as-absent hole this
    // taxonomy closes (review 2026-09-01 §2). An unmapped failure stays
    // EUNKNOWN (transient), at the cost of a rare over-report on a
    // malformed path.
    result.error = FsFailureMessage(stat_error);
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
    std::optional<base::DictValue> stat =
        BuildStatDict(ResolvePath(cwd_, path), nullptr);
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
    // Same taxonomy as Stat, from the same last-error capture. The captured
    // NOT_FOUND leads unconditionally — a PathExists probe can race a
    // concurrent re-create and would then bury the ENOENT inside the
    // parenthetical, where the guest's ANCHORED not-found matcher never
    // looks. Present-but-unreadable carries the discriminated code as a
    // parenthetical DETAIL so existing "Failed to read file" matchers keep
    // working while the guest gains the cause.
    const base::File::Error read_error = base::File::GetLastFileError();
    if (read_error == base::File::FILE_ERROR_NOT_FOUND ||
        !base::PathExists(resolved)) {
      result.error = kEnoentMessage;
    } else {
      result.error = "Failed to read file (" + FsFailureMessage(read_error) + ")";
    }
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

#if NEXUS_FS_ATOMIC_WRITE
  bool written = false;
  if (atomic) {
    // Temp file in the target's directory, then ReplaceFile — a crash at any
    // point leaves either the old file or the new one, never a torn one. The
    // guest asks for this on the session `meta.json`, whose unlogged fields
    // have no other copy.
    written = base::ImportantFileWriter::WriteFileAtomically(
        resolved, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                   bytes.size()));
    if (!written) {
      // Upstream's GUID temp name needs 41 characters of headroom past the
      // directory, so it gives up ~32 characters before this path is actually
      // too long. Retry atomically with a short temp name rather than falling
      // back to a torn-able plain write — see WriteFileAtomicallyShortTemp.
      written = WriteFileAtomicallyShortTemp(resolved, base::span(bytes));
    }
  } else {
    written = base::WriteFile(resolved, base::span(bytes));
  }
#else
  // NEXUS_FS_ATOMIC_WRITE == 0: `atomic` is accepted and ignored, so the guest
  // needs no change and an atomic-capable build can be restored by the flag
  // alone. See the flag's comment for what this gives up.
  (void)atomic;
  bool written = base::WriteFile(resolved, base::span(bytes));
#endif
  if (!written) {
    // Discriminated detail in parentheses (same prefix-preserving shape as
    // ReadFile): a locked/denied write now names its cause instead of the
    // bare ambiguity the stat one call earlier no longer has.
    result.error = "Failed to write file (" +
                   FsFailureMessage(base::File::GetLastFileError()) + ")";
    return result;
  }
  result.success = true;
  return result;
}

NexusFsHost::WriteIfMatchResult NexusFsHost::WriteFileIfMatch(
    const std::string& path,
    mojo_base::BigBuffer content,
    bool atomic,
    const WriteIfMatch& if_match) const {
  WriteIfMatchResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);
  base::File::Info info;
  if (!base::GetFileInfo(resolved, &info) || info.is_directory) {
    result.error_code = kPreconditionFailedCode;
    result.error = "The file no longer exists.";
    return result;
  }
  const double mtime_ms =
      info.last_modified.is_null()
          ? 0
          : info.last_modified.InMillisecondsFSinceUnixEpoch();
  if (static_cast<double>(info.size) != if_match.size ||
      std::abs(mtime_ms - if_match.mtime_ms) > kIfMatchMtimeToleranceMs) {
    result.error_code = kPreconditionFailedCode;
    result.error = "The file changed since it was read (size " +
                   base::NumberToString(info.size) + ", mtimeMs " +
                   base::NumberToString(mtime_ms) + ").";
    return result;
  }

  SimpleResult written = WriteFile(path, std::move(content), atomic);
  if (!written.success) {
    result.error = std::move(written.error);
    return result;
  }
  // The stat the caller's next ifMatch compares against. A failed re-stat
  // (the file vanished at once) is still a successful write.
  base::File::Info after;
  if (base::GetFileInfo(resolved, &after)) {
    result.size = static_cast<double>(after.size);
    result.mtime_ms = after.last_modified.is_null()
                          ? 0
                          : after.last_modified.InMillisecondsFSinceUnixEpoch();
  }
  result.success = true;
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::CopyFile(const std::string& src,
                                                const std::string& dst,
                                                bool overwrite) const {
  SimpleResult result;
  const base::FilePath from = ResolvePath(cwd_, src);
  const base::FilePath to = ResolvePath(cwd_, dst);
  base::File::Info info;
  if (!base::GetFileInfo(from, &info)) {
    result.error = FsFailureMessage(base::File::GetLastFileError());
    return result;
  }
  if (info.is_directory) {
    result.error = "EISDIR: the source is a directory";
    return result;
  }
  if (!overwrite && base::PathExists(to)) {
    result.error = "EEXIST: the destination already exists";
    return result;
  }
  if (!base::CopyFile(from, to)) {
    result.error = "Failed to copy file (" +
                   FsFailureMessage(base::File::GetLastFileError()) + ")";
    return result;
  }
  result.success = true;
  return result;
}

NexusFsHost::SimpleResult NexusFsHost::Symlink(const std::string& target,
                                               const std::string& path) const {
  SimpleResult result;
  const base::FilePath link = ResolvePath(cwd_, path);
  const base::FilePath link_target = base::FilePath::FromUTF8Unsafe(target);
  if (target.empty()) {
    result.error = "EINVAL: the link target is empty";
    return result;
  }
  base::File::Info existing;
  if (base::GetFileInfo(link, &existing)) {
    result.error = "EEXIST: the link path already exists";
    return result;
  }
#if BUILDFLAG(IS_WIN)
  // base::CreateSymbolicLink is POSIX-only. A directory target needs the
  // directory flag; resolve a relative target against the link's directory
  // to find out what it names.
  const base::FilePath resolved_target =
      link_target.IsAbsolute() ? link_target
                               : link.DirName().Append(link_target);
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (base::DirectoryExists(resolved_target)) {
    flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  }
  if (!::CreateSymbolicLinkW(link.value().c_str(), link_target.value().c_str(),
                             flags)) {
    const DWORD error = ::GetLastError();
    result.error =
        error == ERROR_PRIVILEGE_NOT_HELD
            ? "EPERM: creating a symbolic link needs Developer Mode or "
              "elevation on Windows"
            : "Failed to create symbolic link (" +
                  FsFailureMessage(base::File::OSErrorToFileError(error)) +
                  ")";
    return result;
  }
#else
  if (!base::CreateSymbolicLink(link_target, link)) {
    result.error = "Failed to create symbolic link (" +
                   FsFailureMessage(base::File::GetLastFileError()) + ")";
    return result;
  }
#endif
  result.success = true;
  return result;
}

NexusFsHost::ReadlinkResult NexusFsHost::Readlink(
    const std::string& path) const {
  ReadlinkResult result;
  const base::FilePath link = ResolvePath(cwd_, path);
#if BUILDFLAG(IS_WIN)
  const DWORD attributes = ::GetFileAttributesW(link.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    result.error = FsFailureMessage(
        base::File::OSErrorToFileError(::GetLastError()));
    return result;
  }
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    result.error = "EINVAL: not a symbolic link";
    return result;
  }
  base::FilePath target;
  if (!base::NormalizeFilePath(link, &target)) {
    result.error = "Failed to resolve the link target";
    return result;
  }
#else
  base::FilePath target;
  if (!base::ReadSymbolicLink(link, &target)) {
    const base::File::Error error = base::File::GetLastFileError();
    result.error = base::PathExists(link) ? "EINVAL: not a symbolic link"
                                          : FsFailureMessage(error);
    return result;
  }
#endif
  result.success = true;
  result.target = target.AsUTF8Unsafe();
  return result;
}

NexusFsHost::ChmodResult NexusFsHost::Chmod(const std::string& path,
                                            int32_t mode) const {
  ChmodResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);
  if (!base::PathExists(resolved)) {
    result.error = kEnoentMessage;
    return result;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(resolved, mode & 07777)) {
    result.error = "Failed to change permissions (" +
                   FsFailureMessage(base::File::GetLastFileError()) + ")";
    return result;
  }
  result.applied = true;
#else
  // POSIX modes do not exist here; the path exists, so the call succeeds
  // without changing anything and says so.
  (void)mode;
  result.applied = false;
#endif
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

NexusFsHost::SimpleResult NexusFsHost::Rename(
    const std::string& old_path,
    const std::string& new_path) const {
  SimpleResult result;
  const base::FilePath resolved_old = ResolvePath(cwd_, old_path);
  const base::FilePath resolved_new = ResolvePath(cwd_, new_path);

  // The error strings below are a wire contract (mapped to DOMException
  // names guest-side) — change them only together with
  // BrowserFsOperations.rename.
  if (!base::PathExists(resolved_old)) {
    result.error = "Source does not exist";
    return result;
  }
  // Never overwrite. base::Move already refuses an existing destination on
  // Windows, but the POSIX rename(2) underneath it silently replaces a
  // destination file — this check is what makes the contract hold on every
  // platform. (Best-effort: a same-instant external create can still race
  // it, as with any pre-check.)
  if (base::PathExists(resolved_new)) {
    result.error = "Destination already exists";
    return result;
  }
  if (!base::Move(resolved_old, resolved_new)) {
    result.error = "Failed to move path";
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

NexusFsHost::RealpathResult NexusFsHost::Realpath(const std::string& path,
                                                  bool resolve_links) const {
  RealpathResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  // base::MakeAbsoluteFilePath returns an empty FilePath on failure
  // (target missing, permission denied, broken symlink loop). It
  // resolves symlinks on POSIX and normalizes the path on Windows, which
  // is exactly what JS-side dedup wants: two callers that ended up at
  // the same on-disk file produce the same string.
  base::FilePath canonical = base::MakeAbsoluteFilePath(resolved);
  if (resolve_links) {
    // Windows: `canonical` is normalized (`..` collapsed), not link-resolved.
    // POSIX: it is empty for a path that does not exist yet.
    // The caller asked for links resolved because it will compare the result
    // against a containment root: an unresolved fallback would let a link
    // escape pass, so a failure here is an error, never the literal path.
    canonical = ResolveLinks(canonical.empty() ? resolved : canonical);
    if (canonical.empty()) {
      result.error = "Failed to resolve links";
      return result;
    }
  }
  if (canonical.empty()) {
    result.error = "Failed to resolve canonical path";
    return result;
  }

  result.success = true;
  result.path = canonical.AsUTF8Unsafe();
  return result;
}

}  // namespace nexus
