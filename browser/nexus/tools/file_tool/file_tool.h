#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_FILE_TOOL_FILE_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_FILE_TOOL_FILE_TOOL_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/gtest_prod_util.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/mojom/nexus_media.mojom.h"
#include "custom_browser/browser/nexus/tools/nexus_ripgrep_runner.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_macros.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

// Generated category-aggregate base. One *.gen.h per category holds the
// abstract base class (`FileToolBase` here) whose virtuals aggregate
// every tool declared under declarations/file_tool/<name>.tool.json.
// The category header transitively pulls in each per-tool *.gen.h (for
// the typed `<Name>Input` structs the virtuals receive), so a single
// include covers the entire generated-base-class surface.
#include "custom_browser/browser/nexus/tools/file_tool.gen.h"

class Profile;

namespace nexus {

class NexusMediaServiceLauncher;
class NexusShellServiceLauncher;
class ToolSession;  // for CategoryAccessor specialization below

// Per-LLM-session class hosting Claude-Code-style local-filesystem
// tools (`read`, `write`, `edit`, `glob`). Mirrors the role
// `BrowserTool` plays for browser actions: one instance per session,
// owned as a member of `ToolSession`.
//
// Threading: the declarations carry `runtime.thread = "ui"`, so the
// codegen-emitted dispatcher invokes each `Run<Name>OnUI` directly on
// the bridge port service's sequence (UI). The body then owns its own
// thread hop, mirroring how `ProcessTool`'s UI-thread tools fire async
// shell work:
//   (a) on UI — validate inputs and read any session-level state
//       (`GetSession()->cwd()` for Glob); both require the FileTool /
//       ToolSession to be alive, which is guaranteed on the
//       dispatcher's own sequence;
//   (b) `base::ThreadPool::PostTaskAndReplyWithResult` hops the
//       blocking file I/O onto a worker. The work is a *static*
//       `<Name>Blocking` helper (no `this`), so the worker never
//       touches FileTool state — a session torn down mid-I/O cannot be
//       use-after-freed by the worker;
//   (c) the reply `On<Name>Done` runs back on UI, bound through
//       `weak_ptr_factory_.GetWeakPtr()`. If the session ended while
//       the I/O was in flight, the WeakPtr is invalidated and the reply
//       is dropped cleanly (the discarded `ToolCallback`'s destructor
//       emits `tool_dropped`) instead of resolving into a freed object.
//       Resolve/Reject must run on UI anyway because the success/error
//       callbacks inside `done` capture a `base::WeakPtr<ToolsAdapter>`
//       whose home sequence is UI.
//
// Why the body — not the dispatcher — owns the hop: the codegen
// `thread_pool` routing binds `base::Unretained(category)` onto the
// worker task, which dangles if the per-session FileTool is destroyed
// (session deleted / panel closed) before the worker runs — a
// use-after-free observed in the field. Keeping the tools on `ui` and
// hopping only the static blocking helper removes that dangling-pointer
// window entirely. See docs/agents/nexus-tools-api.md §6.
//
// FileTool itself is owned by `ToolSession`, which is owned by
// `ToolsAdapter::sessions_` (UI-thread).
//
// No `Profile`-level state is currently used — the tools operate on
// absolute paths the LLM provides. The sandboxing decision (which
// paths are reachable) is deferred to a future change; for now the
// implementation enforces only the absolute-path invariant.
class FileTool : public ToolBase,
                 public ::nexus::generated::FileToolBase {
 public:
  FileTool(ToolSession* session, Profile* profile,
           const std::string& session_key);
  ~FileTool() override;

  FileTool(const FileTool&) = delete;
  FileTool& operator=(const FileTool&) = delete;

  // No-op for symmetry with `BrowserTool::DefineTools` — every category
  // exposes this static so `ToolCatalog::Get()` can call it. All
  // file-tool registration goes through the codegen-emitted
  // `generated::PopulateRegistry()`; nothing macro-DSL-flavored is
  // registered here.
  static void DefineTools(ToolRegistry& registry);

  // Cancels in-flight reply lambdas via the weak-ptr factory so
  // anything queued on `base::ThreadPool` quietly drops on completion
  // instead of resolving a stale `ToolCallback`. Idempotent.
  void OnStop(OnStopReason reason) override;

  // Overrides of generated::FileToolBase virtuals. The codegen
  // dispatcher invokes these directly on UI (the declarations carry
  // `runtime.thread = "ui"`). Each body validates on UI, then hops the
  // blocking I/O to a base::ThreadPool worker via
  // PostTaskAndReplyWithResult and resolves `done` from the UI reply.
  // Public visibility is required because the codegen dispatcher
  // invokes through a `FileTool*` it gets from
  // `CategoryAccessor::Get(session)`.
  void RunReadOnUI(::nexus::generated::ReadInput input,
                   ToolCallback done) override;
  void RunWriteOnUI(::nexus::generated::WriteInput input,
                    ToolCallback done) override;
  void RunEditOnUI(::nexus::generated::EditInput input,
                   ToolCallback done) override;
  void RunGlobOnUI(::nexus::generated::GlobInput input,
                   ToolCallback done) override;

  // Clears per-session read-state used for Read dedup and Edit/Write stale-read
  // guards. Used after compaction/clear because prior Read content may have been
  // removed from the model context.
  void ClearReadState();

  // Splits a glob `pattern` into (baseDir, relativePattern) at its first
  // glob metacharacter (`* ? [ {`), a faithful port of the reference
  // GlobTool's `extractGlobBaseDirectory` (claude_code utils/glob.ts).
  // `.first` is the forward-slash-normalized static directory prefix
  // (everything up to the last separator before the first glob char; "" when
  // the pattern begins with a glob char or has no separator; "/" or "C:/" for
  // a root). `.second` is the remaining tail.
  //
  // RunGlobOnUI applies this ONLY to ABSOLUTE patterns, to feed ripgrep a
  // concrete search root + a relative `--glob` pattern (rg's `--glob` accepts
  // relative patterns only). Static, pure, and side-effect free; declared
  // here (not in the anonymous namespace) so it can be unit-tested.
  static std::pair<std::string, std::string> ExtractGlobBaseDirectory(
      const std::string& pattern);

 public:
  // Read helpers in file_tool.cc construct this move-only result before the
  // private RunReadOnUI reply trampoline consumes it. Keep the methods private;
  // only the carrier type is public so file-local helpers can name it.
  struct ReadResult {
    ReadResult();
    ReadResult(ReadResult&&) noexcept;
    ReadResult& operator=(ReadResult&&) noexcept;
    ReadResult(const ReadResult&) = delete;
    ReadResult& operator=(const ReadResult&) = delete;
    ~ReadResult();

    bool ok = false;
    std::string error_code;
    std::string error_message;
    // When non-empty, the OnDone trampoline forwards `message` as a
    // plain-string result to the bridge — the model sees CC-style
    // prose like "File created successfully at: <path>". Otherwise
    // `payload` is forwarded as a Dict (legacy / structured-telemetry
    // path). Exactly one of the two should be populated on success.
    std::string message;
    base::DictValue payload;
    base::FilePath file_path;
    std::string full_content;
    base::Time last_modified;
    bool is_partial_view = false;
    std::optional<int> offset;
    std::optional<int> limit;
    bool cache_read_state = false;
  };

 private:
  // ---------------- Worker-thread helpers ----------------
  //
  // Pure free-function-shaped statics that the ThreadPool worker
  // invokes. They take all inputs by value / const ref, do blocking
  // file I/O, and return a typed result struct that the UI-side reply
  // lambda consumes. Keeping them static (no `this`) means workers
  // never touch FileTool's mutable state — the WeakPtr binding only
  // gates the reply path. UTF-8 string find/replace is byte-safe
  // because UTF-8 is self-synchronizing (a multi-byte sequence can't
  // be a substring of a different one).

  // Per-method result structs. Each carries either a payload dict (on
  // success) or an error code + message (on failure); the reply
  // lambda forwards exactly one of those into the `ToolCallback`.
  //
  // Each struct is move-only (base::DictValue is move-only) and must
  // declare its special members out-of-line per chromium-style — the
  // clang plugin flags inline-defined ctor/dtor on any class with
  // non-trivial member types like std::string / base::Value::Dict.
  // Definitions live in file_tool.cc as `= default;`.

  struct WriteResult {
    WriteResult();
    WriteResult(WriteResult&&) noexcept;
    WriteResult& operator=(WriteResult&&) noexcept;
    WriteResult(const WriteResult&) = delete;
    WriteResult& operator=(const WriteResult&) = delete;
    ~WriteResult();

    bool ok = false;
    std::string error_code;
    std::string error_message;
    // When non-empty, the OnDone trampoline forwards `message` as a
    // plain-string result to the bridge — the model sees CC-style
    // prose like "File created successfully at: <path>". Otherwise
    // `payload` is forwarded as a Dict (legacy / structured-telemetry
    // path). Exactly one of the two should be populated on success.
    std::string message;
    base::DictValue payload;
    // Pre-edit file contents (CC's FileWriteOutput original content). Set only
    // when overwriting an existing file (not on create). When `set`, OnWriteDone
    // forwards a dict `{ message, original_file }` instead of a bare prose
    // string; the JS hostedTools.ts splits that into the model-facing prose +
    // a UI-only file-relative diff. `original_file` NEVER reaches the model.
    std::string original_file;
    bool original_file_set = false;
    base::FilePath file_path;
    std::string updated_content;
    base::Time last_modified;
  };

  struct EditResult {
    EditResult();
    EditResult(EditResult&&) noexcept;
    EditResult& operator=(EditResult&&) noexcept;
    EditResult(const EditResult&) = delete;
    EditResult& operator=(const EditResult&) = delete;
    ~EditResult();

    bool ok = false;
    std::string error_code;
    std::string error_message;
    // When non-empty, the OnDone trampoline forwards `message` as a
    // plain-string result to the bridge — the model sees CC-style
    // prose like "File created successfully at: <path>". Otherwise
    // `payload` is forwarded as a Dict (legacy / structured-telemetry
    // path). Exactly one of the two should be populated on success.
    std::string message;
    base::DictValue payload;
    // Pre-edit file contents (CC's FileEditOutput.originalFile). Always set on
    // a successful edit. When `set`, OnEditDone forwards a dict
    // `{ message, original_file }` instead of a bare prose string; the JS
    // hostedTools.ts splits that into the model-facing prose + a UI-only
    // file-relative diff. `original_file` NEVER reaches the model.
    std::string original_file;
    bool original_file_set = false;
    base::FilePath file_path;
    std::string updated_content;
    base::Time last_modified;
  };

  struct ReadState {
    std::string content;
    base::Time last_modified;
    bool is_partial_view = false;
    std::optional<int> offset;
    std::optional<int> limit;
    bool from_read = false;
  };

  static ReadResult ReadFileBlocking(base::FilePath path,
                                     std::optional<int> offset,
                                     std::optional<int> limit,
                                     std::optional<std::string> pages,
                                     std::optional<int> context_window,
                                     std::optional<ReadState> last_read);

  static WriteResult WriteFileBlocking(base::FilePath path,
                                       std::string content,
                                       std::optional<ReadState> last_read);

  static EditResult EditFileBlocking(base::FilePath path,
                                     std::string old_string,
                                     std::string new_string,
                                     bool replace_all,
                                     std::optional<ReadState> last_read);

  // ---------------- UI-thread reply trampolines ----------------
  //
  // Read/Write/Edit hop blocking file I/O to a base::ThreadPool worker and
  // resolve from these, bound through `weak_ptr_factory_.GetWeakPtr()` so a
  // session that ends during I/O cleanly drops the reply (the discarded
  // ToolCallback emits `tool_dropped`) instead of resolving into freed
  // memory. Each forwards the typed result into a single Resolve / Reject.
  void OnReadDone(ToolCallback done, ReadResult result);
  void OnReadMediaDone(base::FilePath path,
                     ToolCallback done,
                     bool ok,
                     ::nexus::mojom::MediaReadResultPtr result,
                     const std::optional<std::string>& error_code,
                     const std::optional<std::string>& error_message);
  void OnWriteDone(ToolCallback done, WriteResult result);
  void OnEditDone(ToolCallback done, EditResult result);

  // Continuation for `NexusRipgrepRunner::Run` from `RunGlobOnUI`. Unlike the
  // Read/Write/Edit trampolines above, Glob shells out to ripgrep through the
  // shared runner (CC GlobTool parity — `rg --files --glob`) rather than the
  // thread-pool path. Shapes the runner's parsed `lines` into the CC-style
  // "No files found" / newline-joined / truncation-caveat result, or rejects
  // a pre-launch failure (rg unavailable after the runner's auto-install).
  void OnGlobRgDone(ToolCallback done, int limit, RipgrepRunResult result);

  // Lets the notebook-read-state regression test drive the media reply
  // trampoline directly (the real path crosses a utility-process boundary that
  // the unit-test harness can’t spin up) and inspect the resulting read-state.
  FRIEND_TEST_ALL_PREFIXES(FileToolTest,
                           NotebookReadRecordsReadStateEnablingWrite);

  std::map<base::FilePath, ReadState> read_state_;

  // Per-session lazily-bound launcher for the sandboxed NexusShellService,
  // created in the ctor. Glob shells `rg` through it (Read/Write/Edit do
  // direct file I/O and don't use it). `OnStop` shuts it down on teardown.
  // ProcessTool keeps its own launcher; per-session double-launching is the
  // documented tradeoff (idle-recycled after 30s) of not threading a shared
  // launcher through ToolSession — see ProcessTool's class comment.
  std::unique_ptr<NexusShellServiceLauncher> launcher_;
  std::unique_ptr<NexusMediaServiceLauncher> media_launcher_;

  // Shared rg orchestrator backing the Glob tool (exec + auto-install + line
  // parsing). Borrows `launcher_`; `OnStop` resets it so in-flight mojo /
  // installer callbacks drop cleanly on teardown.
  NexusRipgrepRunner ripgrep_runner_;

  // chromium-style requires WeakPtrFactory to be the last member of the
  // outer class so it's destroyed first (invalidating all outstanding
  // WeakPtrs before any other member's destructor runs and risks a
  // use-after-free from a still-pending callback). The Run<Name>OnUI
  // bodies call `weak_ptr_factory_.GetWeakPtr()` on UI — the same
  // sequence as the factory — to bind each PostTaskAndReplyWithResult
  // reply, so a session destroyed mid-I/O drops the reply instead of
  // resolving into freed memory.
  base::WeakPtrFactory<FileTool> weak_ptr_factory_{this};
};

}  // namespace nexus

// Declare the macro DSL's category accessor for `FileTool`. The
// matching `NEXUS_DEFINE_CATEGORY(FileTool, file_tool)` lives in
// `file_tool.cc`, which can include `tool_session.h` (the header
// can't, because `tool_session.h` includes this file to embed
// `FileTool` as a member).
NEXUS_DECLARE_CATEGORY(nexus::FileTool);

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_FILE_TOOL_FILE_TOOL_H_
