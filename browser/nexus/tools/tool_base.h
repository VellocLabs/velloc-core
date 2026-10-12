#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_

#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"

class Profile;

namespace nexus {

class ToolSession;

// Typed reason for the OnStop cascade, parsed from guest strings by
// `ParseOnStopReason`. kCompleted / kUserStopped keep the ToolSession warm;
// kSessionDeleted alone erases it (~BrowserTool closes its tabs);
// kDisconnected / kShutdown are port-service / profile teardown; kDestructor
// is the idempotent fallback.
enum class OnStopReason {
  kCompleted,
  kUserStopped,
  kSessionDeleted,
  kDisconnected,
  kShutdown,
  kDestructor,
};

// Parse a JS-facing reason string into the typed enum. Unrecognized /
// empty strings fall back to `fallback` (callers typically pass
// `kUserStopped` because that's the safe default the JS guest used
// before the typed contract existed).
OnStopReason ParseOnStopReason(std::string_view reason,
                                OnStopReason fallback = OnStopReason::kUserStopped);

// Stable short name for logs / telemetry. Mirrors the legacy reason
// strings so existing greps over `[NexusTrace]` logs still match.
std::string_view OnStopReasonToString(OnStopReason reason);

// `ToolBase` — common base for every per-session tool category, holding
// (ToolSession*, Profile*, session_key) and the OnStop hook (default empty).
// OnStop must be idempotent: ~ToolSession calls it again if none ran first.
// `GetSession()` is non-null for the category's lifetime.
class ToolBase {
 public:
  ToolBase(ToolSession* session, Profile* profile,
           const std::string& session_key);
  virtual ~ToolBase();

  ToolBase(const ToolBase&) = delete;
  ToolBase& operator=(const ToolBase&) = delete;

  // Common accessors used by tool method implementations.
  ToolSession* GetSession() const { return session_.get(); }
  Profile* GetProfile() const { return profile_.get(); }
  const std::string& GetSessionKey() const { return session_key_; }

  // Where a tool posts its blocking work: the session's sequence
  // (ToolSession::blocking_runner), or a private sequence when the tool was
  // built without a session (unit tests). Never run blocking work on the
  // UI entry.
  scoped_refptr<base::SequencedTaskRunner> BlockingRunner() const;

  // Called when the LLM session is stopping. The cascade is driven by
  // `ToolSessionRegistry::OnSessionStopped(session_key, reason)` →
  // `ToolSession::OnStop(reason)` → each per-category member's
  // `OnStop(reason)`. See `OnStopReason` above for per-value semantics.
  //
  // Implementations MUST be idempotent — OnStop may run more than once
  // in some teardown paths (e.g. explicit kCompleted then destructor's
  // kDestructor). Default implementation is a no-op.
  virtual void OnStop(OnStopReason reason);

 private:
  raw_ptr<ToolSession> session_;
  raw_ptr<Profile> profile_;
  std::string session_key_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_
