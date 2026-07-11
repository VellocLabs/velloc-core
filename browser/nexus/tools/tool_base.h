#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_

#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"

class Profile;

namespace nexus {

class ToolSession;

// Typed reason for the OnStop cascade. Replaces the pre-refactor
// stringly-typed `const std::string& reason` parameter so the C++
// side is exhaustive over reasons and a typo can't silently fall
// through to a default.
//
// JS-facing reason strings are parsed at the bridge boundary
// (`NexusSessionBridgeAdapter::HandleOnStopOnUI`) by
// `ParseOnStopReason`. Internal callers (destructors, port-service
// teardown) pass enum values directly.
//
// Semantics:
//   - kCompleted       — conversation finished naturally (one per turn,
//                          fired by `query/deps.ts::handleStopHooks`).
//                          Stops the actor task; leaves ToolSession in
//                          the adapter map so the next turn reuses it.
//   - kUserStopped     — user pressed Stop. Same lifetime as kCompleted.
//   - kSessionDeleted  — user deleted the session from the sidebar.
//                          ToolsAdapter erases the ToolSession on this
//                          reason ONLY; ~BrowserTool then closes the
//                          session's tabs.
//   - kDisconnected    — bridge port service teardown (panel close /
//                          webview reload). `~ToolsAdapter` cascades
//                          this before the sessions map clears, which
//                          triggers ~BrowserTool → tab close.
//   - kShutdown        — profile / process teardown. Same behavior as
//                          kDisconnected.
//   - kDestructor      — defensive call from a destructor when no
//                          explicit OnStop ran first. Idempotent guard.
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

// `ToolBase` — common base for every per-LLM-session tool category
// (BrowserTool, future SkillTool, etc.). Holds the shared (ToolSession*,
// Profile*, session_key) trio every category needs and exposes the
// OnStop() notification hook the framework cascades when a session is
// being torn down.
//
// Subclasses override OnStop to clean up category-specific state
// (BrowserTool stops its actor::TaskId; future categories do their
// own teardown). The default OnStop is empty — categories that don't
// need cleanup don't have to override.
//
// Lifetime contract: a ToolBase subclass is a member of a ToolSession,
// which is owned by ToolsAdapter::sessions_. When the session ends —
// either via an explicit endSession RPC, a port-service tear-down, or
// profile shutdown — the framework calls OnStop(reason) before the
// member's destructor runs. OnStop must be idempotent because
// destructor-time cleanup is also a defensive fallback path
// (~ToolSession invokes OnStop("destructor") on every member if no
// explicit OnStop happened first).
//
// The `session` raw_ptr lets a category read session-level state
// (cwd, action_timeout, …) without category-specific plumbing.
// `GetSession()` is non-null for the lifetime of the tool category
// instance because ToolSession owns the category by value (members
// outlive the session no longer than `*this` does).
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

  // Called when the LLM session is stopping. The cascade is driven by
  // `ToolsAdapter::OnSessionStopped(session_key, reason)` →
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
