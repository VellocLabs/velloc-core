#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

// Synchronous filesystem helper used by NexusFsBridgeAdapter. Each
// method performs blocking I/O on the calling thread and returns a
// typed result struct — there are no callbacks and no internal
// PostTasks. The caller is responsible for being on a thread that
// allows blocking; NexusFsBridgeAdapter satisfies that by hopping each
// call to base::ThreadPool (MayBlock) via PostTaskAndReplyWithResult.
//
// RefCountedThreadSafe because that worker task holds a scoped_refptr to
// the host: a torn-down adapter (port-service teardown racing an
// in-flight fs op) must not free the host out from under the worker.
// The host is immutable after construction (just `cwd_`), so sharing it
// across threads read-only is safe; the last ref may be dropped on any
// thread, which the trivial destructor allows.
//
// The one exception to "no internal PostTasks" is the constructor: it
// posts a single `CreateDirectory(cwd)` task to thread_pool so the
// working directory exists before any per-call work runs. Doing it
// synchronously in the ctor would block the UI thread (where NexusFsHost
// is constructed); leaving the cwd un-created until first use would race
// with concurrent calls.
class NexusFsHost : public base::RefCountedThreadSafe<NexusFsHost> {
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

  struct RealpathResult {
    bool success = false;
    // Canonical (symlink-resolved on POSIX, normalized on Windows) path,
    // UTF-8 encoded. Empty when `success` is false.
    std::string path;
    std::string error;
  };

  // Host-process platform identifiers. All three fields are intentionally
  // simple strings — the guest just renders them verbatim into the
  // `# Environment` block of its system prompt. Empty fields mean
  // "unknown"; the guest drops the corresponding line from the prompt
  // rather than guessing.
  struct SystemInfoResult {
    // 'win32' | 'darwin' | 'linux', matching Node's process.platform
    // strings so the guest's `Platform:` line is byte-identical to
    // Claude Code desktop's.
    std::string platform;
    // Human-readable OS version, e.g. "Windows 11 Pro 10.0.26200" /
    // "macOS 14.4 23E214" / "Linux 6.6.4".
    std::string os_version;
    // Interpreter name only, no path. Derived from $SHELL (POSIX) or
    // $COMSPEC (Windows). Empty when neither env var is set.
    std::string shell;
  };

  // ----------------------------------------------------------------------

  explicit NexusFsHost(const base::FilePath& cwd);

  NexusFsHost(const NexusFsHost&) = delete;
  NexusFsHost& operator=(const NexusFsHost&) = delete;

  // The cwd is set once at construction; reading it is thread-safe and
  // non-blocking, so this is the only method callable from UI.
  std::string Cwd() const;

  // Returns the OS user's home directory (HOME on POSIX / USERPROFILE on
  // Windows), as resolved by base::PathService::Get(base::DIR_HOME).
  // Used by the WebUI bridge so per-user settings (e.g. feature toggles)
  // carry across browser profiles + projects — anchored to the OS user,
  // not the chromium profile's user-data-dir. Empty string when no home
  // dir is discoverable (very rare; some service-account setups).
  // Resolved via PathService so the chromium test harness can override
  // it through ScopedPathOverride for test isolation.
  std::string UserHome() const;

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
  // Resolve `path` to its canonical absolute form (follows symlinks on
  // POSIX, normalizes `..` segments and drive case on Windows). Used by
  // the JS guest's skill-dedup pipeline so two SKILL.md files surfaced
  // through different sources collapse when they ultimately point at the
  // same on-disk file.
  RealpathResult Realpath(const std::string& path) const;

  // Snapshot host-process platform info — `base::SysInfo` reads on the
  // current thread, so callers must allow base::MayBlock() like the
  // other methods in this class.
  SystemInfoResult SystemInfo() const;

 private:
  friend class base::RefCountedThreadSafe<NexusFsHost>;
  ~NexusFsHost();

  base::FilePath cwd_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_
