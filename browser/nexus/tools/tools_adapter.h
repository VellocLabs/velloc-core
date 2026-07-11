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
//   1. Extracts `executionContext.sessionId` from the payload (via
//      `ExtractSessionKey`).
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

  // Records that a tool dispatch is starting. Bumps `total_invocations_`
  // and `running_count_`, remembers the tool name keyed by request `id`,
  // and broadcasts `nexusAgentStateChanged`. Called from the dispatcher
  // handler, where the tool's `schema.name` is in scope.
  void OnToolDispatchStarted(const std::string& id,
                             const std::string& tool_name);

 private:
  // Iterates `ToolCatalog::Get().entries()` and registers each as a
  // session-routing bridge handler.
  void RegisterCatalogDispatchers();

  // Resolves the pending tool entry for `id` (set in
  // `OnToolDispatchStarted`), decrements `running_count_`, optionally
  // bumps `successful_invocations_`, and broadcasts the new agent state.
  void OnToolDispatchFinished(const std::string& id, bool success);

  // Builds the `nexusAgentStateChanged` payload from the current counters
  // and pushes it to every WebUI in this profile via
  // `BroadcastWebUiListenerEventToProfile`.
  void BroadcastAgentState();

  // Registered under the `browser` capability as the `endSession`
  // action. Reads `executionContext.sessionId` (or top-level
  // `sessionId`) plus an optional string `reason`, runs the OnStop
  // cascade through the matching `ToolSession`, drops the session
  // from the map, and resolves with `{ ok: true, stopped: <bool> }`.
  //
  // This is the canonical way for the JS side to invoke the tool
  // framework's OnStop hook. `BrowserTool::OnStop` stops the actor
  // task and clears the per-session element registry as part of the
  // cleanup, so a single `browser.endSession` call is enough to wind
  // a session down end-to-end. The handler is idempotent — calling it
  // twice for the same session_key is safe (the second call returns
  // `stopped: false`).
  void HandleEndSession(const std::string& id,
                        const base::DictValue& route,
                        const base::DictValue& payload,
                        std::vector<blink::MessagePortDescriptor> ports);

  // Registered under the `browser` capability as
  // `setAgentLoopSettings`. Pushes JS-side AgentLoopSettings
  // (`actionTimeoutMs`, `stepTimeoutMs`) onto the matching `ToolSession`. Lazily
  // creates the session if absent so JS can push settings before the
  // first tool call. Resolves with the applied values after C++-side clamping.
  void HandleSetAgentLoopSettings(
      const std::string& id,
      const base::DictValue& route,
      const base::DictValue& payload,
      std::vector<blink::MessagePortDescriptor> ports);

  raw_ptr<Profile> profile_;
  std::map<std::string, std::unique_ptr<ToolSession>> sessions_;

  // Aggregate tool-call telemetry surfaced to the topbar via
  // `nexusAgentStateChanged`. Aggregated across all sessions served by
  // this adapter (one per `NexusBridgePortService`, i.e. one per
  // webview); reset implicitly when the adapter is destroyed.
  int total_invocations_ = 0;
  int successful_invocations_ = 0;
  int running_count_ = 0;
  std::string current_tool_name_;
  std::map<std::string, std::string> pending_tool_names_;

  base::WeakPtrFactory<ToolsAdapter> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOLS_ADAPTER_H_
