#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
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
    result.error = "Failed to read file";
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

}  // namespace nexus
