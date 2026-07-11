#include "custom_browser/browser/nexus/fs/nexus_fs_host.h"

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

base::FilePath ResolvePath(const base::FilePath& cwd,
                           const std::string& path_str) {
  const base::FilePath requested = base::FilePath::FromUTF8Unsafe(path_str);
  if (requested.IsAbsolute()) {
    return requested;
  }
  return cwd.Append(requested);
}

std::vector<uint8_t> CopyBigBufferBytes(const mojo_base::BigBuffer& buffer) {
  std::vector<uint8_t> bytes(buffer.size());
  base::span(bytes).copy_from(base::span(buffer));
  return bytes;
}

struct StatResult {
  bool success = false;
  std::string json;
  std::string error;
};

StatResult PopulateStatResult(const base::FilePath& path,
                              [[maybe_unused]] bool follow_symlinks) {
  StatResult result;
  base::File::Info info;
  bool ok = false;
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  if (follow_symlinks) {
    ok = base::GetFileInfo(path, &info);
  } else {
    base::stat_wrapper_t stat_info;
    ok = base::File::Lstat(path, &stat_info) == 0;
    if (ok) {
      info.FromStat(stat_info);
    }
  }
#else
  ok = base::GetFileInfo(path, &info);
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

StatResult DoStatOnThreadPool(base::FilePath path) {
  return PopulateStatResult(path, true);
}

struct ReadDirResult {
  bool success = false;
  std::string json;
  std::string error;
};

ReadDirResult DoReadDirOnThreadPool(base::FilePath path) {
  ReadDirResult result;
  if (!base::DirectoryExists(path)) {
    result.error = "Path is not a directory";
    return result;
  }

  base::ListValue list;
  base::FileEnumerator enumerator(
      path, false,
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

struct ReadResult {
  bool success = false;
  std::optional<mojo_base::BigBuffer> data;
  std::string error;
};

ReadResult DoReadFileOnThreadPool(base::FilePath path) {
  ReadResult result;
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(path);
  if (!bytes.has_value()) {
    result.error = "Failed to read file";
    return result;
  }

  result.success = true;
  result.data.emplace(base::span(*bytes));
  return result;
}

struct ReadRangeResult {
  bool success = false;
  std::optional<mojo_base::BigBuffer> data;
  uint32_t bytes_read = 0;
  std::string error;
};

ReadRangeResult DoReadRangeOnThreadPool(base::FilePath path,
                                        int64_t offset,
                                        uint32_t length) {
  ReadRangeResult result;
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    result.error = "Failed to open file";
    return result;
  }

  std::vector<uint8_t> buffer(length);
  const std::optional<size_t> read_bytes = file.Read(offset, base::span(buffer));
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

struct SimpleResult {
  bool success = false;
  std::string error;
};

SimpleResult DoWriteFileOnThreadPool(base::FilePath path,
                                     std::vector<uint8_t> content) {
  SimpleResult result;
  if (!base::WriteFile(path, base::span(content))) {
    result.error = "Failed to write file";
    return result;
  }
  result.success = true;
  return result;
}

SimpleResult DoAppendFileOnThreadPool(base::FilePath path,
                                      std::vector<uint8_t> data) {
  SimpleResult result;
  base::File file(path, base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
  if (!file.IsValid()) {
    result.error = "Failed to open file for append";
    return result;
  }
  if (!file.WriteAtCurrentPosAndCheck(base::span(data))) {
    result.error = "Failed to append file";
    return result;
  }
  result.success = true;
  return result;
}

SimpleResult DoMkdirOnThreadPool(base::FilePath path) {
  SimpleResult result;
  if (!base::CreateDirectory(path)) {
    result.error = "Failed to create directory";
    return result;
  }
  result.success = true;
  return result;
}

SimpleResult DoRmOnThreadPool(base::FilePath path,
                              bool recursive,
                              bool force) {
  SimpleResult result;
  if (!base::PathExists(path)) {
    result.success = force;
    if (!force) {
      result.error = "Path does not exist";
    }
    return result;
  }

  const bool ok = recursive ? base::DeletePathRecursively(path)
                            : base::DeleteFile(path);
  if (!ok) {
    result.error = recursive ? "Failed to remove path recursively"
                             : "Failed to remove path";
    return result;
  }

  result.success = true;
  return result;
}

SimpleResult DoUnlinkOnThreadPool(base::FilePath path) {
  SimpleResult result;
  if (!base::DeleteFile(path)) {
    result.error = "Failed to unlink path";
    return result;
  }
  result.success = true;
  return result;
}

struct BoolResult {
  bool success = false;
  bool value = false;
  std::string error;
};

BoolResult DoExistsOnThreadPool(base::FilePath path) {
  BoolResult result;
  result.success = true;
  result.value = base::PathExists(path);
  return result;
}

}  // namespace

NexusFsHost::NexusFsHost(const base::FilePath& cwd) : cwd_(cwd) {
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](const base::FilePath& path) { base::CreateDirectory(path); },
          cwd_));
}

NexusFsHost::~NexusFsHost() = default;

void NexusFsHost::Cwd(CwdCallback callback) {
  std::move(callback).Run(cwd_.AsUTF8Unsafe());
}

void NexusFsHost::Stat(const std::string& path, StatCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoStatOnThreadPool, ResolvePath(cwd_, path)),
      base::BindOnce(
          [](StatCallback cb, StatResult result) {
            std::move(cb).Run(
                result.success, result.json,
                result.success ? std::nullopt
                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::Readdir(const std::string& path, ReaddirCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoReadDirOnThreadPool, ResolvePath(cwd_, path)),
      base::BindOnce(
          [](ReaddirCallback cb, ReadDirResult result) {
            std::move(cb).Run(
                result.success, result.json,
                result.success ? std::nullopt
                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::ReadFile(const std::string& path, ReadFileCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoReadFileOnThreadPool, ResolvePath(cwd_, path)),
      base::BindOnce(
          [](ReadFileCallback cb, ReadResult result) {
            std::move(cb).Run(
                result.success, std::move(result.data),
                result.success ? std::nullopt
                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::ReadRange(const std::string& path,
                            uint64_t offset,
                            uint32_t length,
                            ReadRangeCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoReadRangeOnThreadPool, ResolvePath(cwd_, path),
                     static_cast<int64_t>(offset), length),
      base::BindOnce(
          [](ReadRangeCallback cb, ReadRangeResult result) {
            std::move(cb).Run(
                result.success, std::move(result.data), result.bytes_read,
                result.success ? std::nullopt
                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::WriteFile(const std::string& path,
                            mojo_base::BigBuffer content,
                            WriteFileCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoWriteFileOnThreadPool, ResolvePath(cwd_, path),
                     CopyBigBufferBytes(content)),
      base::BindOnce(
          [](WriteFileCallback cb, SimpleResult result) {
            std::move(cb).Run(
                result.success, result.success ? std::nullopt
                                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::AppendFile(const std::string& path,
                             mojo_base::BigBuffer data,
                             uint32_t /*mode*/,
                             AppendFileCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoAppendFileOnThreadPool, ResolvePath(cwd_, path),
                     CopyBigBufferBytes(data)),
      base::BindOnce(
          [](AppendFileCallback cb, SimpleResult result) {
            std::move(cb).Run(
                result.success, result.success ? std::nullopt
                                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::Mkdir(const std::string& path,
                        uint32_t /*mode*/,
                        MkdirCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoMkdirOnThreadPool, ResolvePath(cwd_, path)),
      base::BindOnce(
          [](MkdirCallback cb, SimpleResult result) {
            std::move(cb).Run(
                result.success, result.success ? std::nullopt
                                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::Rm(const std::string& path,
                     bool recursive,
                     bool force,
                     RmCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoRmOnThreadPool, ResolvePath(cwd_, path), recursive,
                     force),
      base::BindOnce(
          [](RmCallback cb, SimpleResult result) {
            std::move(cb).Run(
                result.success, result.success ? std::nullopt
                                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::Unlink(const std::string& path, UnlinkCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoUnlinkOnThreadPool, ResolvePath(cwd_, path)),
      base::BindOnce(
          [](UnlinkCallback cb, SimpleResult result) {
            std::move(cb).Run(
                result.success, result.success ? std::nullopt
                                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

void NexusFsHost::Exists(const std::string& path, ExistsCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DoExistsOnThreadPool, ResolvePath(cwd_, path)),
      base::BindOnce(
          [](ExistsCallback cb, BoolResult result) {
            std::move(cb).Run(
                result.success, result.value,
                result.success ? std::nullopt
                               : std::make_optional(result.error));
          },
          std::move(callback)));
}

}  // namespace nexus
