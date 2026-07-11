#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOLS_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOLS_ADAPTER_H_

#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"

class Profile;

namespace nexus {

class NexusBridgeResponseDelegate;

// `ToolsAdapter` — bridges the global `ToolCatalog` to a per-port-service
// dispatch table.
//
// One instance per `NexusBridgePortService`. Owns a per-LLM-session map
// of `ToolSession` instances; each `ToolSession` aggregates one
// `BrowserTool` (and future per-category classes) that hold the actual
// per-session state (e.g. `actor::TaskId`).
//
// On construction, iterates `ToolCatalog::Get().entries()` and registers
// a session-routing bridge handler for each tool. Each handler:
//   1. Extracts `executionContext.sessionId` from the payload (via the
//      file-local `ExtractSessionKey` in tools_adapter.cc).
//   2. Looks up (or lazily creates) the matching `ToolSession`.
//   3. Runs the catalog entry's dispatcher with `&session` as the
//      instance; the dispatcher's inner trampoline uses
//      `CategoryAccessor<Category>::Get(session)` to descend to the
//      per-category member, then runs the typed extraction + method
//      invocation.
//
// Session lifecycle:
//   - `GetOrCreateSession(key)` — lazy create on first use.
//   - `OnSessionStopped(key, reason)` — runs the OnStop cascade
//     (`ToolSession::OnStop` → each per-category `OnStop`) so e.g.
//     `BrowserTool::OnStop` stops its actor task. Then erases the
//     session from the map.
//   - `~ToolsAdapter` — runs `OnStop("disconnected")` on every
//     remaining session as a defensive fallback (port-service
//     teardown / webview reload path).
class ToolsAdapter : public NexusBridgeCapabilityHandler {
 public:
  ToolsAdapter(NexusBridgeResponseDelegate* delegate, Profile* profile);
  ~ToolsAdapter() override;

  ToolsAdapter(const ToolsAdapter&) = delete;
  ToolsAdapter& operator=(const ToolsAdapter&) = delete;

  // Looks up an existing session or creates one keyed by session_key.
  // The first call from a new sessionId mints a fresh `ToolSession`
  // (and therefore a fresh per-category state, e.g. a fresh
  // `BrowserTool` with no `actor::TaskId` yet).
  ToolSession& GetOrCreateSession(const std::string& session_key);

  // True iff a session keyed by `session_key` is currently in the map.
  // Used by `NexusSessionBridgeAdapter::HandleOnStopOnUI` to surface
  // the `{ ok, stopped }` shape — `stopped` is `true` only when the
  // session was live at the time of the call. Phase 12f exposed this
  // method so the bridge-side `onStop` handler (which lives outside
  // ToolsAdapter) can read the answer without reaching into the
  // private `sessions_` map.
  bool HasSession(const std::string& session_key) const;

  // Cascades the OnStop notification through the session and erases
  // it from the map. Idempotent — no-op if `session_key` isn't present.
  void OnSessionStopped(const std::string& session_key,
                        const std::string& reason);

  // Forwarders used by per-tool `ToolCallback` resolutions. Public so
  // `base::BindOnce(&ToolsAdapter::ForwardSuccess, weak_ptr, …)` can
  // target them.
  void ForwardSuccess(std::string id,
                      base::DictValue route,
                      base::Value result);
  void ForwardError(std::string id,
                    base::DictValue route,
                    std::string code,
                    std::string message);

  // Success forwarder that ALSO wraps the tool result with `agent_meta`
  // (action_hashes, page_fingerprint, post_url, post_focus_target_id,
  // terminates_sequence) before forwarding. Reads the per-session
  // last-observation state via `BrowserTool::last_observation_*`
  // accessors. Bound at the dispatch site so each invocation captures
  // its own (tool_name, args, session_key) tuple. Older JS-side
  // builds that don't parse `agent_meta` see only the `data` field;
  // newer builds get the full envelope.
  void ForwardSuccessWithAgentMeta(std::string id,
                                   base::DictValue route,
                                   std::string tool_name,
                                   base::DictValue args,
                                   std::string session_key,
                                   base::Value result);

 private:
  // Iterates `ToolCatalog::Get().entries()` and registers each as a
  // session-routing bridge handler.
  //
  // Phase 12f moved the manual `endSession` and `setAgentLoopSettings`
  // handlers off this adapter onto the codegen-driven `session`
  // capability (see `api/session/nexus_session_bridge_adapter.{h,cc}`).
  // ToolsAdapter now ONLY hosts the model-facing tool dispatch — every
  // entry registered here is an LLM-callable browser tool.
  void RegisterCatalogDispatchers();

  raw_ptr<Profile> profile_;
  std::map<std::string, std::unique_ptr<ToolSession>> sessions_;

  base::WeakPtrFactory<ToolsAdapter> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOLS_ADAPTER_H_
