#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_FS_NEXUS_FS_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_FS_NEXUS_FS_HOST_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

// Handles system-level filesystem operations on behalf of the Nexus UI.
class NexusFsHost {
 public:
  using CwdCallback = base::OnceCallback<void(const std::string& cwd)>;
  using StatCallback =
      base::OnceCallback<void(bool success,
                              const std::string& json_stat,
                              std::optional<std::string> error_message)>;
  using ReaddirCallback =
      base::OnceCallback<void(bool success,
                              const std::string& json_dirents,
                              std::optional<std::string> error_message)>;
  using ReadFileCallback =
      base::OnceCallback<void(bool success,
                              std::optional<mojo_base::BigBuffer> data,
                              std::optional<std::string> error_message)>;
  using ReadRangeCallback =
      base::OnceCallback<void(bool success,
                              std::optional<mojo_base::BigBuffer> data,
                              uint32_t bytes_read,
                              std::optional<std::string> error_message)>;
  using WriteFileCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> error_message)>;
  using AppendFileCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> error_message)>;
  using MkdirCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> error_message)>;
  using RmCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> error_message)>;
  using UnlinkCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> error_message)>;
  using ExistsCallback =
      base::OnceCallback<void(bool success,
                              bool exists,
                              std::optional<std::string> error_message)>;

  explicit NexusFsHost(const base::FilePath& cwd);
  ~NexusFsHost();

  NexusFsHost(const NexusFsHost&) = delete;
  NexusFsHost& operator=(const NexusFsHost&) = delete;

  void Cwd(CwdCallback callback);
  void Stat(const std::string& path, StatCallback callback);
  void Readdir(const std::string& path, ReaddirCallback callback);
  void ReadFile(const std::string& path, ReadFileCallback callback);
  void ReadRange(const std::string& path,
                 uint64_t offset,
                 uint32_t length,
                 ReadRangeCallback callback);
  void WriteFile(const std::string& path,
                 mojo_base::BigBuffer content,
                 WriteFileCallback callback);
  void AppendFile(const std::string& path,
                  mojo_base::BigBuffer data,
                  uint32_t mode,
                  AppendFileCallback callback);
  void Mkdir(const std::string& path,
             uint32_t mode,
             MkdirCallback callback);
  void Rm(const std::string& path,
          bool recursive,
          bool force,
          RmCallback callback);
  void Unlink(const std::string& path, UnlinkCallback callback);
  void Exists(const std::string& path, ExistsCallback callback);

 private:
  base::FilePath cwd_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_FS_NEXUS_FS_HOST_H_
