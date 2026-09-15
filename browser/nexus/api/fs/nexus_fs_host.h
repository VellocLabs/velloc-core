#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"

// Atomic (temp-file + replace) writes for `fs.writeFile({atomic: true})`.
//
// DISABLED while we investigate a Microsoft Defender false positive on the
// released installer. Defender's cloud ML flagged the NSIS package
// (Trojan:Script/Sabsik.FL.A!ml, later Trojan:Win32/Wacatac.B!ml); a build with
// this path compiled out scanned clean side-by-side with one that had it, so it
// is switched off until the installer is code-signed. Signing is the real fix:
// these are unsigned-binary reputation verdicts, and every inner file
// (chrome.dll, mini_installer.exe) scans clean on its own.
//
// Re-enable by flipping this to 1 — that restores
// `base::ImportantFileWriter::WriteFileAtomically` plus the short-temp-name
// fallback, and nothing outside this pair of files has to change (the guest
// keeps sending `atomic` either way; a 0 build accepts and ignores it).
//
// What is given up while it is 0: a crash midway through rewriting a session's
// `meta.json` can tear the file. The guest rebuilds titles/counts/preview from
// `transcript.jsonl`, but the unlogged fields — model selection, permission
// mode, composer draft, counters, terminal state — live only in that file.
#define NEXUS_FS_ATOMIC_WRITE 0

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

  // ---- Batched results ---------------------------------------------------
  //
  // These three carry `base::ListValue` rather than a JSON string. The
  // single-path methods above hand back a string that the adapter
  // immediately re-parses (RespondJson) — free at one row, wasteful at the
  // ~2000 rows a session-store scan produces. base::Value is move-only, so
  // each struct needs the same out-of-line ctor/move/dtor treatment as
  // ReadResult.

  struct StatBatchResult {
    // `success` is whole-call only; a path that cannot be stat'd is a row
    // with a null `stat`, not a failure.
    bool success = false;
    base::ListValue rows;  // [{path, stat: {...}|null}, ...]
    std::string error;

    StatBatchResult();
    StatBatchResult(StatBatchResult&&) noexcept;
    StatBatchResult& operator=(StatBatchResult&&) noexcept;
    StatBatchResult(const StatBatchResult&) = delete;
    StatBatchResult& operator=(const StatBatchResult&) = delete;
    ~StatBatchResult();
  };

  struct ReaddirTreeResult {
    bool success = false;
    base::ListValue entries;  // [{path, isDirectory, size, mtimeMs?}, ...]
    bool truncated = false;
    std::string root;
    std::string error;

    ReaddirTreeResult();
    ReaddirTreeResult(ReaddirTreeResult&&) noexcept;
    ReaddirTreeResult& operator=(ReaddirTreeResult&&) noexcept;
    ReaddirTreeResult(const ReaddirTreeResult&) = delete;
    ReaddirTreeResult& operator=(const ReaddirTreeResult&) = delete;
    ~ReaddirTreeResult();
  };

  // One requested read. `length == 0` means "to end of file" from `offset`.
  struct ReadBatchItem {
    std::string path;
    uint64_t offset = 0;
    uint32_t length = 0;
  };

  struct ReadBatchResult {
    bool success = false;
    // One entry per requested item, in request order, each echoing its
    // `path` and indexing into `data`:
    //   {path, ok, byteOffset, byteLength, error?}
    base::ListValue entries;
    std::optional<mojo_base::BigBuffer> data;  // all items concatenated
    bool truncated = false;                    // byte budget stopped the loop
    std::string error;

    ReadBatchResult();
    ReadBatchResult(ReadBatchResult&&) noexcept;
    ReadBatchResult& operator=(ReadBatchResult&&) noexcept;
    ReadBatchResult(const ReadBatchResult&) = delete;
    ReadBatchResult& operator=(const ReadBatchResult&) = delete;
    ~ReadBatchResult();
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
    // Canonical (symlink-resolved on POSIX, normalized on Windows — or
    // link-resolved everywhere with `resolve_links`) path, UTF-8 encoded.
    // Empty when `success` is false.
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
    // Interpreter name only, no path: the shell the model's shell tool
    // runs (host_shell.h ChooseReportedShellName). Empty when unknown.
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

  // Stat every path in one pass. Rows are 1:1 with `paths` and in the
  // same order (duplicates preserved), each echoing the REQUEST string —
  // not the resolved path — so callers can key results by what they asked
  // for. An unreadable path yields `stat: null` rather than aborting.
  StatBatchResult StatBatch(const std::vector<std::string>& paths) const;

  // Breadth-first listing of `path`, descending at most `max_depth` levels
  // and emitting at most `max_entries` rows. Directories AT the depth cap
  // are LISTED but not descended — that cap is what keeps a session-store
  // scan from walking every per-session tool-details/ file. Row paths are
  // root-relative and '/'-separated on every platform (a wire contract the
  // guest splits on). Callers are expected to have clamped both bounds.
  ReaddirTreeResult ReaddirTree(const std::string& path,
                                int32_t max_depth,
                                int32_t max_entries) const;

  // Read every item into ONE concatenated buffer, with an index describing
  // each item's slice. A response can carry only a single buffer, so this
  // is how a batched read is expressible at all. Items that fail are
  // marked `ok:false` and skipped; they do not abort the batch. When the
  // byte budget is exhausted the remaining items are marked failed and
  // `truncated` is set — a partial answer beats none for a sidebar.
  ReadBatchResult ReadBatch(const std::vector<ReadBatchItem>& items,
                            size_t max_total_bytes,
                            size_t max_item_bytes) const;
  ReadResult ReadFile(const std::string& path) const;
  ReadRangeResult ReadRange(const std::string& path,
                            uint64_t offset,
                            uint32_t length) const;
  // `atomic`: write via a temp file that then replaces `path`
  // (base::ImportantFileWriter::WriteFileAtomically), so a crash mid-write
  // never leaves a partially written target. The default is the in-place
  // base::WriteFile — unchanged behaviour for every caller that does not ask.
  SimpleResult WriteFile(const std::string& path,
                         mojo_base::BigBuffer content,
                         bool atomic = false) const;
  SimpleResult AppendFile(const std::string& path,
                          mojo_base::BigBuffer data) const;
  SimpleResult Mkdir(const std::string& path) const;
  // Rename/move a file or directory via base::Move — a true rename on the
  // same volume (atomic, O(1), carries whole directory trees), an internal
  // copy+delete only across volumes. NEVER overwrites: an existing
  // destination fails with the exact string "Destination already exists",
  // and a missing source with "Source does not exist" — the JS guest maps
  // these strings to AlreadyExistsError / NotFoundError (see
  // BrowserFsOperations.rename), so they are wire contract, not prose.
  SimpleResult Rename(const std::string& old_path,
                      const std::string& new_path) const;
  SimpleResult Rm(const std::string& path, bool recursive, bool force) const;
  SimpleResult Unlink(const std::string& path) const;
  ExistsResult Exists(const std::string& path) const;
  // Resolve `path` to its canonical absolute form (follows symlinks on
  // POSIX, normalizes `..` segments and drive case on Windows). Used by
  // the JS guest's skill-dedup pipeline so two SKILL.md files surfaced
  // through different sources collapse when they ultimately point at the
  // same on-disk file.
  //
  // `resolve_links` asks for the path with links resolved on every platform:
  // on Windows that follows junctions, symlinks and mapped / subst drives
  // (GetFinalPathNameByHandle), which the default form does not. It is for
  // containment checks — the untrusted read boundary and the fsx root rule —
  // where a link inside a root that points out of it is the escape to catch.
  // A path that does not exist yet resolves the links of its deepest existing
  // ancestor and keeps the rest. When the link-resolved form cannot be had
  // (a path over MAX_PATH, a target that cannot be opened) the default form is
  // returned. The default is kept for everything else on purpose: it feeds
  // the session cwd and the session-storage identity, which must not move for
  // a project opened through a junction.
  RealpathResult Realpath(const std::string& path, bool resolve_links) const;

  // Snapshot host-process platform info — `base::SysInfo` reads on the
  // current thread, so callers must allow base::MayBlock() like the
  // other methods in this class.
  SystemInfoResult SystemInfo() const;

 private:
  friend class base::RefCountedThreadSafe<NexusFsHost>;
  ~NexusFsHost();

  base::FilePath cwd_;
};

// Map a base::File::Error to the discriminated, Node-style wire message —
// THE single producer of the fs error-string taxonomy (a wire contract: the
// guest's `isFsNotFoundError` keys on the leading code; see
// nexus_web utils/fsTransportFailure.ts). Every discriminating method calls
// this so a new code is added in exactly one place. Exposed at namespace
// scope so the unit test can pin each mapping without staging
// platform-specific failures.
//
// Unmapped errors deliberately emit the raw enum VALUE, not
// base::File::ErrorToString — those tokens are chromium-versioned and must
// never ride a wire string the guest pattern-matches.
std::string FsFailureMessage(base::File::Error error);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_HOST_H_
