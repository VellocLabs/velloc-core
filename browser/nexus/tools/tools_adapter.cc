#include "custom_browser/browser/nexus/tools/tools_adapter.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_signature.h"
#include "custom_browser/browser/nexus/tools/tool_catalog.h"

namespace nexus {

namespace {

// Pull the session id out of a tool-dispatch payload. Two accepted
// shapes:
//   * preferred (what the JS guest's `buildHostedToolExecutionContext`
//     produces): `{ executionContext: { sessionId: "..." } }`
//   * legacy: top-level `{ sessionId: "..." }`
// A missing or empty session id falls through to the "__default__"
// sentinel so single-session callers don't have to thread the field
// every call. Multi-session callers always populate it.
//
// Phase 12f shrank the call sites: this helper now only services the
// model-facing tool dispatcher in `RegisterCatalogDispatchers`. The
// session-lifecycle path (`session.onStop`,
// `session.setAgentLoopSettings`) lives in
// `api/session/nexus_session_bridge_adapter.cc` and reads the same
// shape via the codegen-extracted typed input.
std::string ExtractSessionKey(const base::DictValue& payload) {
  if (const base::DictValue* ctx = payload.FindDict("executionContext")) {
    if (const std::string* session_id = ctx->FindString("sessionId")) {
      if (!session_id->empty()) {
        return *session_id;
      }
    }
  }
  if (const std::string* top_level = payload.FindString("sessionId")) {
    if (!top_level->empty()) {
      return *top_level;
    }
  }
  return "__default__";
}

}  // namespace

ToolsAdapter::ToolsAdapter(NexusBridgeResponseDelegate* delegate,
                            Profile* profile)
    : NexusBridgeCapabilityHandler(delegate), profile_(profile) {
  // Phase 12f: the session-lifecycle handlers (`endSession`,
  // `setAgentLoopSettings`) moved to `NexusSessionBridgeAdapter`.
  // ToolsAdapter only owns the model-facing tool dispatch table.
  RegisterCatalogDispatchers();
}

ToolsAdapter::~ToolsAdapter() {
  // Defensive cascade — the port service is being torn down (e.g.
  // webview reload, side panel close). Notify every still-live session
  // so they get a final chance to stop their actor tasks. ~ToolSession
  // also runs OnStop(kDestructor) as a redundant fallback; sessions'
  // OnStop overrides are idempotent. After this returns, the sessions
  // map clears, destroying each ToolSession → ~BrowserTool closes that
  // session's tagged tabs ("tabs close with the panel" UX).
  for (auto& [key, session] : sessions_) {
    if (session) {
      session->OnStop(OnStopReason::kDisconnected);
    }
  }
}

ToolSession& ToolsAdapter::GetOrCreateSession(
    const std::string& session_key) {
  auto it = sessions_.find(session_key);
  if (it != sessions_.end()) {
    return *it->second;
  }
  auto session = std::make_unique<ToolSession>(profile_, session_key);
  ToolSession* raw = session.get();
  sessions_.emplace(session_key, std::move(session));
  return *raw;
}

bool ToolsAdapter::HasSession(const std::string& session_key) const {
  return sessions_.find(session_key) != sessions_.end();
}

void ToolsAdapter::OnSessionStopped(const std::string& session_key,
                                     OnStopReason reason) {
  auto it = sessions_.find(session_key);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second) {
    it->second->OnStop(reason);
  }
  // The session is removed from the map ONLY when the user explicitly
  // deleted it. For kCompleted / kUserStopped / kDisconnected / kShutdown
  // the ToolSession (and its BrowserTool + node registry + session-tab
  // bookkeeping) stays warm so the next turn / Stop-then-resume reuses
  // it. The next tool call's `EnsureTaskCreated` re-arms the actor task
  // (task_id_ was reset inside BrowserTool::OnStop).
  if (reason == OnStopReason::kSessionDeleted) {
    sessions_.erase(it);
  }
}

void ToolsAdapter::ForwardSuccess(std::string id,
                                   base::DictValue route,
                                   base::Value result) {
  // Tool-state telemetry to the topbar is now driven JS-side by
  // `SharedWorkerSessionManager.markToolEnd`, which fires the unified
  // `nexusCurrentSessionState` broadcast. The C++ side just forwards
  // the bridge response.
  SendSuccessResponse(id, route, std::move(result));
}

void ToolsAdapter::ForwardSuccessWithAgentMeta(std::string id,
                                                base::DictValue route,
                                                std::string tool_name,
                                                base::DictValue args,
                                                std::string session_key,
                                                base::Value result) {
  AgentToolMeta meta;
  // action_hashes and terminates_sequence depend only on tool_name + args;
  // page_fingerprint / post_url / post_focus_target_id come from the
  // session's BrowserTool last-observation snapshot, populated inside
  // `BuildObservationDict` / `WalkContentNode`.
  meta.action_hashes = ComputeActionHashes(tool_name, args);
  meta.terminates_sequence = IsSequenceTerminator(tool_name);

  auto it = sessions_.find(session_key);
  if (it != sessions_.end() && it->second) {
    const BrowserTool& bt = it->second->browser_tool();
    if (!bt.last_observation_url().empty()) {
      meta.page_url = bt.last_observation_url();
      meta.page_interactive_count = bt.last_observation_interactive_count();
      meta.page_dom_hash =
          ComputeDomHash(bt.last_observation_normalized_projection());
      meta.post_url = bt.last_observation_url();
    }
    if (bt.has_last_observation_focus_target()) {
      meta.post_focus_target_present = true;
      meta.post_focus_target_id = bt.last_observation_focus_target_id();
    }
  }

  base::Value wrapped = WrapResultWithAgentMeta(std::move(result), meta);
  ForwardSuccess(std::move(id), std::move(route), std::move(wrapped));
}

void ToolsAdapter::ForwardError(std::string id,
                                 base::DictValue route,
                                 std::string code,
                                 std::string message) {
  SendErrorResponse(id, route, code, message);
}

void ToolsAdapter::RegisterCatalogDispatchers() {
  base::WeakPtr<ToolsAdapter> weak_self = weak_ptr_factory_.GetWeakPtr();
  for (const ToolEntry& entry : ToolCatalog::Get().entries()) {
    DispatchHandler handler =
        [weak_self, dispatcher = entry.dispatcher,
         tool_name = entry.schema.name](
            const std::string& id, const base::DictValue& route,
            const base::DictValue& payload,
            std::vector<blink::MessagePortDescriptor> /*ports*/) {
          ToolsAdapter* adapter = weak_self.get();
          if (!adapter) {
            return;
          }

          // Look up (or create) the per-session container for this
          // request. Tools that don't carry a sessionId end up bucketed
          // under ExtractSessionKey's "__default__" fallback.
          std::string session_key = ExtractSessionKey(payload);
          ToolSession& session = adapter->GetOrCreateSession(session_key);

          // Tool-state telemetry to the topbar is now driven JS-side by
          // `SharedWorkerSessionManager.markToolStart/End` and routed
          // through the `ui.broadcastCurrentSessionState` bridge action.
          // No native counter bookkeeping here.

          // Per-request callback bound to (id, route, weak adapter).
          // Each invocation gets a fresh ToolCallback; the dispatcher's
          // inner trampoline drives extraction + method invocation on
          // the per-session category instance.
          //
          // The success forwarder is `ForwardSuccessWithAgentMeta` so
          // every tool result gets the agent-loop envelope appended
          // (action_hashes from tool_name + args; page_fingerprint /
          // post_url / post_focus_target_id from the session's
          // BrowserTool last-observation snapshot;
          // terminates_sequence from a static tool-name set).
          base::DictValue route_for_success = route.Clone();
          base::DictValue route_for_error = route.Clone();
          auto on_success = base::BindOnce(
              &ToolsAdapter::ForwardSuccessWithAgentMeta,
              weak_self, id, std::move(route_for_success),
              tool_name, payload.Clone(), session_key);
          auto on_error = base::BindOnce(&ToolsAdapter::ForwardError,
                                          weak_self, id,
                                          std::move(route_for_error));
          ToolCallback callback(std::move(on_success), std::move(on_error));

          dispatcher.Run(&session, payload.Clone(), std::move(callback));
        };
    RegisterHandler(entry.schema.name, std::move(handler));
  }
}

}  // namespace nexus
