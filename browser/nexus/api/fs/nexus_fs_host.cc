#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

#include <stdint.h>

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
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
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
  const base::FilePath resolved = ResolvePath(cwd_, path);

  base::File::Info info;
  bool ok = false;
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  // Always Lstat — match the previous follow_symlinks=true case via
  // GetFileInfo when symlink resolution is desired; the bridge protocol
  // currently never asks for lstat-only behavior, so keep parity with
  // the pre-refactor code (which called PopulateStatResult with
  // follow_symlinks=true).
  ok = base::GetFileInfo(resolved, &info);
#else
  ok = base::GetFileInfo(resolved, &info);
#endif
  if (!ok) {
    result.error = "File not found or permission denied";
    return result;
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

  result.success = true;
  base::JSONWriter::Write(base::Value(std::move(dict)), &result.json);
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
  const base::FilePath resolved = ResolvePath(cwd_, path);

  base::File file(resolved, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    result.error = "Failed to open file";
    return result;
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
    mojo_base::BigBuffer content) const {
  SimpleResult result;
  const base::FilePath resolved = ResolvePath(cwd_, path);

  if (!base::WriteFile(resolved, base::span(CopyBigBufferBytes(content)))) {
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
