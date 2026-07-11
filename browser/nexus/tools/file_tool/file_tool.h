#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_FILE_TOOL_FILE_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_FILE_TOOL_FILE_TOOL_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
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

class ToolSession;  // for CategoryAccessor specialization below

// Per-LLM-session class hosting Claude-Code-style local-filesystem
// tools (`read`, `write`, `edit`). Mirrors the role `BrowserTool` plays
// for browser actions: one instance per session, owned as a member of
// `ToolSession`.
//
// Threading: the codegen-emitted dispatcher in
// nexus_tools_registry.gen.cc handles the thread hop. Because the
// declarations carry `runtime.thread = "thread_pool"`, the dispatcher
// calls `base::ThreadPool::PostTask({base::MayBlock(),
// base::TaskPriority::USER_VISIBLE}, ...)` to invoke each
// `Run<Name>OnThreadPool` on a ThreadPool worker. The body then:
//   (a) validates inputs and does the blocking file I/O on the
//       worker — both already-correct-thread, no extra hop;
//   (b) PostTasks back to `content::GetUIThreadTaskRunner({})` to
//       invoke `On<Name>Done`, which calls `done.Resolve` /
//       `done.Reject`. The Resolve/Reject path is UI-bound because
//       the captured success/error callbacks inside `done` use a
//       `base::WeakPtr<ToolsAdapter>` whose home sequence is UI.
//
// FileTool itself is owned by `ToolSession`, which is owned by
// `ToolsAdapter::sessions_` (UI-thread). The reply trampolines bind
// `weak_this_` so a session that ends mid-flight cleanly drops the
// reply instead of dereferencing freed memory. `weak_this_` is
// cached at construction time (UI) so worker-thread bodies can
// capture it without ever calling `weak_ptr_factory_.GetWeakPtr()`
// off-sequence — `WeakPtrFactory` is sequence-bound to its
// constructor's thread.
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
  // dispatcher PostTasks these onto a base::ThreadPool worker before
  // invoking, so each body executes on a worker thread (NOT UI). The
  // body does its blocking I/O directly and then PostTasks back to
  // UI for the `done.Resolve` / `done.Reject` step. Public visibility
  // is required because the codegen dispatcher invokes through a
  // `FileTool*` it gets from `CategoryAccessor::Get(session)`.
  void RunReadOnThreadPool(::nexus::generated::ReadInput input,
                           ToolCallback done) override;
  void RunWriteOnThreadPool(::nexus::generated::WriteInput input,
                            ToolCallback done) override;
  void RunEditOnThreadPool(::nexus::generated::EditInput input,
                           ToolCallback done) override;
  void RunGlobOnThreadPool(::nexus::generated::GlobInput input,
                           ToolCallback done) override;

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
  };

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
  };

  struct GlobResult {
    GlobResult();
    GlobResult(GlobResult&&) noexcept;
    GlobResult& operator=(GlobResult&&) noexcept;
    GlobResult(const GlobResult&) = delete;
    GlobResult& operator=(const GlobResult&) = delete;
    ~GlobResult();

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
  };

  static ReadResult ReadFileBlocking(base::FilePath path,
                                     std::optional<int> offset,
                                     std::optional<int> limit);

  static WriteResult WriteFileBlocking(base::FilePath path,
                                       std::string content);

  static EditResult EditFileBlocking(base::FilePath path,
                                     std::string old_string,
                                     std::string new_string,
                                     bool replace_all);

  static GlobResult GlobFilesBlocking(base::FilePath root,
                                      std::string pattern,
                                      int limit);

  // ---------------- UI-thread reply trampolines ----------------
  //
  // Bound onto `weak_this_` so a session that ends during I/O cleanly
  // drops the reply. Each one forwards the typed result into a single
  // Resolve / Reject on the captured `done`.
  void OnReadDone(ToolCallback done, ReadResult result);
  void OnWriteDone(ToolCallback done, WriteResult result);
  void OnEditDone(ToolCallback done, EditResult result);
  void OnGlobDone(ToolCallback done, GlobResult result);

  // Cached at construction (on UI) so worker-thread bodies can capture
  // a WeakPtr without calling `weak_ptr_factory_.GetWeakPtr()` from
  // the wrong sequence. The dereference still happens on UI (the
  // PostTask back to `content::GetUIThreadTaskRunner({})` is what
  // invokes On<Name>Done), which is what WeakPtr requires.
  //
  // Initialized in the ctor BODY (not the init list) so chromium-style's
  // "WeakPtrFactory must be last member" rule is satisfied — at body-
  // entry time, weak_ptr_factory_ has already been default-constructed
  // and is safe to call.
  base::WeakPtr<FileTool> weak_this_;

  // chromium-style requires WeakPtrFactory to be the last member of the
  // outer class so it's destroyed first (invalidating all outstanding
  // WeakPtrs before any other member's destructor runs and risks a
  // use-after-free from a still-pending callback).
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
