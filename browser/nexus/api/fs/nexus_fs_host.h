#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

// Synchronous filesystem helper used by NexusFsBridgeAdapter. Each
// method performs blocking I/O on the calling thread and returns a
// typed result struct — there are no callbacks and no internal
// PostTasks. The caller is responsible for being on a thread that
// allows blocking; the bridge adapter satisfies that contract by
// declaring `runtime.thread = "thread_pool"` on every fs *.api.json,
// which makes the codegen-emitted dispatcher PostTask the Handle*()
// virtuals onto base::ThreadPool with MayBlock() before invoking them.
//
// The one exception is the constructor: it posts a single
// `CreateDirectory(cwd)` task to thread_pool so the working directory
// exists before any per-call work runs. Doing it synchronously in the
// ctor would block the UI thread (where NexusFsHost is typically
// constructed); leaving the cwd un-created until first use would race
// with concurrent calls. The startup hop is the only PostTask in this
// class.
class NexusFsHost {
 public:
  // ---- Result structs ----------------------------------------------------
  // One per method. `success`/`error` are uniform; the per-method payload
  // (json, data, etc.) sits alongside. Move-only when they hold
  // BigBuffers; copyable otherwise. Keeping them as plain aggregates
  // avoids needing out-of-line ctor/dtor.

  struct StatResult {
    bool success = false;
    std::string json;
    std::string error;
  };

  struct ReaddirResult {
    bool success = false;
    std::string json;
    std::string error;
  };

  struct ReadResult {
    bool success = false;
    std::optional<mojo_base::BigBuffer> data;
    std::string error;

    // BigBuffer is move-only.
    ReadResult();
    ReadResult(ReadResult&&) noexcept;
    ReadResult& operator=(ReadResult&&) noexcept;
    ReadResult(const ReadResult&) = delete;
    ReadResult& operator=(const ReadResult&) = delete;
    ~ReadResult();
  };

  struct ReadRangeResult {
    bool success = false;
    std::optional<mojo_base::BigBuffer> data;
    uint32_t bytes_read = 0;
    std::string error;

    ReadRangeResult();
    ReadRangeResult(ReadRangeResult&&) noexcept;
    ReadRangeResult& operator=(ReadRangeResult&&) noexcept;
    ReadRangeResult(const ReadRangeResult&) = delete;
    ReadRangeResult& operator=(const ReadRangeResult&) = delete;
    ~ReadRangeResult();
  };

  struct SimpleResult {
    bool success = false;
    std::string error;
  };

  struct ExistsResult {
    bool success = false;
    bool exists = false;
    std::string error;
  };

  // ----------------------------------------------------------------------

  explicit NexusFsHost(const base::FilePath& cwd);
  ~NexusFsHost();

  NexusFsHost(const NexusFsHost&) = delete;
  NexusFsHost& operator=(const NexusFsHost&) = delete;

  // The cwd is set once at construction; reading it is thread-safe and
  // non-blocking, so this is the only method callable from UI.
  std::string Cwd() const;

  // The remaining methods are blocking. Caller MUST be on a thread that
  // allows base::MayBlock().
  StatResult Stat(const std::string& path) const;
  ReaddirResult Readdir(const std::string& path) const;
  ReadResult ReadFile(const std::string& path) const;
  ReadRangeResult ReadRange(const std::string& path,
                            uint64_t offset,
                            uint32_t length) const;
  SimpleResult WriteFile(const std::string& path,
                         mojo_base::BigBuffer content) const;
  SimpleResult AppendFile(const std::string& path,
                          mojo_base::BigBuffer data) const;
  SimpleResult Mkdir(const std::string& path) const;
  SimpleResult Rm(const std::string& path, bool recursive, bool force) const;
  SimpleResult Unlink(const std::string& path) const;
  ExistsResult Exists(const std::string& path) const;

 private:
  base::FilePath cwd_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_
