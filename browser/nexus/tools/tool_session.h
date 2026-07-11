#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_SESSION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_SESSION_H_

#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"
#include "custom_browser/browser/nexus/tools/file_tool/file_tool.h"
#include "custom_browser/browser/nexus/tools/process_tool/process_tool.h"

class Profile;

namespace nexus {

// `ToolSession` — per-LLM-session container. Owns one instance of every
// per-category tool class (currently just `BrowserTool`; future
// categories — skill tools, etc. — add a member here).
//
// Created lazily by `ToolsAdapter::GetOrCreateSession(session_key)` on
// the first tool call carrying a new sessionId. Destroyed when the
// session ends (explicit endSession RPC, bridge port reset, or profile
// shutdown).
//
// `OnStop(reason)` cascades the stop notification to every per-category
// member so each can clean up its own state (e.g. `BrowserTool::OnStop`
// stops the actor task). The destructor runs `OnStop("destructor")` as
// a defensive fallback if no explicit OnStop preceded it.
class ToolSession {
 public:
  ToolSession(Profile* profile, std::string session_key);
  ~ToolSession();

  ToolSession(const ToolSession&) = delete;
  ToolSession& operator=(const ToolSession&) = delete;

  // Per-category accessors. Used by Phase C's macro-DSL category
  // accessor specialization (and by Phase A/B forwarders on the
  // adapter). Returns mutable refs because tool method bodies modify
  // per-category state.
  BrowserTool& browser_tool() { return browser_tool_; }
  FileTool& file_tool() { return file_tool_; }
  ProcessTool& process_tool() { return process_tool_; }
  // SkillTool& skill_tool() { return skill_tool_; }   // future

  Profile* profile() const { return profile_.get(); }
  const std::string& session_key() const { return session_key_; }

  // Cascades the OnStop notification to every per-category member.
  // Called from `ToolsAdapter::OnSessionStopped(session_key, reason)`
  // before the session is erased from the sessions map. The dtor also
  // runs `OnStop("destructor")` as a defensive fallback.
  //
  // Idempotent — every category's `OnStop` override must tolerate
  // being called more than once.
  void OnStop(const std::string& reason);

  // Per-action wall-clock budget. Used by individual tool methods that
  // wrap their actor work (PerformActions / observation builds) with a
  // timer to avoid dead CDP/WebSocket hangs. JS pushes its configured
  // value via the `setAgentLoopSettings` bridge route at engine
  // construction; if no value has been pushed, the getter returns the
  // 180s default (mirrors the Browser-Use reference).
  //
  // Mutating from any thread is unsafe — callers must set this on the
  // bridge thread before any tool runs. Most production paths set
  // once at session start.
  base::TimeDelta action_timeout() const { return action_timeout_; }
  void set_action_timeout(base::TimeDelta timeout);
  base::TimeDelta step_timeout() const { return step_timeout_; }
  void set_step_timeout(base::TimeDelta timeout);

  // Session-level current working directory. Read by tool bodies that
  // need a default cwd for shell / fs operations (Bash, Glob, Grep,
  // and any future tool whose call payload doesn't carry an absolute
  // path). Set via the existing `setAgentLoopSettings` bridge route
  // (parsed in `ToolsAdapter::HandleSetAgentLoopSettings`).
  //
  // The setter only accepts absolute paths — relative paths are
  // dropped silently with a [ToolSession] log entry. An unset cwd
  // returns an empty `base::FilePath()`; tool bodies are expected to
  // surface a clean error rather than silently falling back to the
  // process cwd, which is rarely what the agent's host wants.
  const base::FilePath& cwd() const { return cwd_; }
  void set_cwd(base::FilePath cwd);

 private:
  raw_ptr<Profile> profile_;
  std::string session_key_;
  // Default matches Browser-Use's `action_timeout=180s` per
  // D:/project/broswer_use/browser-use-agent-implementation-report.md
  // "Key Defaults".
  base::TimeDelta action_timeout_ = base::Seconds(180);
  base::TimeDelta step_timeout_ = base::Seconds(180);

  // Default-empty so the unset state is observable by tool bodies.
  // Populated by `set_cwd(...)` from the bridge.
  base::FilePath cwd_;

  // Each per-category class is a member. Construction order is
  // declaration order; destruction reverses. ToolSession::OnStop
  // explicitly cascades so cleanup happens in a controlled order
  // (and at a point in lifetime where Profile is guaranteed alive).
  //
  // Order rationale: BrowserTool first because it owns actor-task
  // teardown that may call into Profile's actor service; FileTool
  // second because its OnStop only invalidates WeakPtrs and has no
  // cross-category dependencies; ProcessTool last because its only
  // outbound dep is the NexusShellService mojo Remote (no other
  // category dependency).
  BrowserTool browser_tool_;
  FileTool file_tool_;
  ProcessTool process_tool_;
  // SkillTool skill_tool_;   // future
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_SESSION_H_
