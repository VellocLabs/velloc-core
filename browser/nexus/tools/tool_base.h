#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_

#include <string>

#include "base/memory/raw_ptr.h"

class Profile;

namespace nexus {

// `ToolBase` — common base for every per-LLM-session tool category
// (BrowserTool, future SkillTool, etc.). Holds the shared (Profile*,
// session_key) pair every category needs and exposes the OnStop()
// notification hook the framework cascades when a session is being
// torn down.
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
class ToolBase {
 public:
  ToolBase(Profile* profile, const std::string& session_key);
  virtual ~ToolBase();

  ToolBase(const ToolBase&) = delete;
  ToolBase& operator=(const ToolBase&) = delete;

  // Common accessors used by tool method implementations.
  Profile* GetProfile() const { return profile_.get(); }
  const std::string& GetSessionKey() const { return session_key_; }

  // Called when the LLM session is stopping. The cascade is driven by
  // `ToolsAdapter::OnSessionStopped(session_key, reason)` →
  // `ToolSession::OnStop(reason)` → each per-category member's
  // `OnStop(reason)`.
  //
  // Reasons (informational, surfaced in [NexusTrace] logs):
  //   - "user_stopped" — explicit user stop / new conversation
  //   - "completed"    — conversation finished naturally
  //   - "disconnected" — bridge port reset / webview reload
  //   - "shutdown"     — profile / process tear-down
  //   - "destructor"   — defensive call from ~ToolSession when no
  //                      explicit OnStop ran first
  //
  // Implementations MUST be idempotent — OnStop may run twice in some
  // teardown paths (e.g. explicit endSession then port reset). Default
  // implementation is a no-op.
  virtual void OnStop(const std::string& reason);

 private:
  raw_ptr<Profile> profile_;
  std::string session_key_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_BASE_H_
