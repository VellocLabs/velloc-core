#include "custom_browser/browser/nexus/tools/tools_adapter.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_extract.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_signature.h"
#include "custom_browser/browser/nexus/tools/tool_catalog.h"
#include "custom_browser/browser/nexus/ui/nexus_ui_broadcast.h"

namespace nexus {

ToolsAdapter::ToolsAdapter(NexusBridgeResponseDelegate* delegate,
                            Profile* profile)
    : NexusBridgeCapabilityHandler(delegate), profile_(profile) {
  RegisterCatalogDispatchers();
  // Session lifecycle action — JS calls `browser.endSession` to
  // cleanly trigger the OnStop cascade for one sessionId.
  Register("endSession", &ToolsAdapter::HandleEndSession);
  // Agent-loop tunables push — JS calls `browser.setAgentLoopSettings`
  // once per session to push `actionTimeoutMs` (and future fields)
  // into the matching ToolSession. See agent-loop integration plan
  // §C in C:/Users/myyyb/.claude/plans/ for the full design.
  Register("setAgentLoopSettings", &ToolsAdapter::HandleSetAgentLoopSettings);
}

ToolsAdapter::~ToolsAdapter() {
  // Defensive cascade — the port service is being torn down (e.g.
  // webview reload). Notify every still-live session so they get a
  // final chance to stop their actor tasks. ~ToolSession also runs
  // OnStop("destructor") as a redundant fallback; sessions' OnStop
  // overrides are idempotent.
  for (auto& [key, session] : sessions_) {
    if (session) {
      session->OnStop("disconnected");
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

void ToolsAdapter::OnSessionStopped(const std::string& session_key,
                                     const std::string& reason) {
  auto it = sessions_.find(session_key);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second) {
    it->second->OnStop(reason);
  }
  sessions_.erase(it);
}

void ToolsAdapter::ForwardSuccess(std::string id,
                                   base::DictValue route,
                                   base::Value result) {
  OnToolDispatchFinished(id, /*success=*/true);
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
  OnToolDispatchFinished(id, /*success=*/false);
  SendErrorResponse(id, route, code, message);
}

void ToolsAdapter::OnToolDispatchStarted(const std::string& id,
                                          const std::string& tool_name) {
  pending_tool_names_[id] = tool_name;
  ++total_invocations_;
  ++running_count_;
  current_tool_name_ = tool_name;
  BroadcastAgentState();
}

void ToolsAdapter::OnToolDispatchFinished(const std::string& id,
                                           bool success) {
  // The dispatch may resolve after the adapter has stopped tracking it
  // (e.g. webview reload races); a missing entry just means there's
  // nothing to credit.
  auto it = pending_tool_names_.find(id);
  if (it == pending_tool_names_.end()) {
    return;
  }
  pending_tool_names_.erase(it);

  if (success) {
    ++successful_invocations_;
  }
  // Defensive clamp — `pending_tool_names_` membership is the source of
  // truth for "is this resolution counted", but `running_count_` is
  // tracked separately to avoid map-size linear scans.
  running_count_ = std::max(0, running_count_ - 1);
  BroadcastAgentState();
}

void ToolsAdapter::BroadcastAgentState() {
  // Tool category is encoded in the schema-name prefix — every browser-
  // category tool starts with `browser_` (see `BrowserTool::DefineTools`
  // in `browser_tool.h`). The topbar uses this flag to decide whether to
  // transiently reveal the agent zone for the duration of the tool call.
  const bool is_browser_tool =
      current_tool_name_.starts_with("browser_");

  base::DictValue payload;
  payload.Set("running", running_count_ > 0);
  payload.Set("toolName", current_tool_name_);
  payload.Set("isBrowserTool", is_browser_tool);
  payload.Set("successful", successful_invocations_);
  payload.Set("total", total_invocations_);
  BroadcastWebUiListenerEventToProfile(
      profile_, "nexusAgentStateChanged", base::Value(std::move(payload)));
}

void ToolsAdapter::HandleSetAgentLoopSettings(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  std::string session_key = ExtractSessionKey(payload);

  // Lazily create the session so JS can push settings before the
  // first tool call. The OnStop cascade still cleans this up if the
  // session is later torn down without ever running a tool.
  ToolSession& session = GetOrCreateSession(session_key);

  // `actionTimeoutMs` may arrive as int or double depending on JS
  // serialization. Try int first, fall back to double.
  std::optional<int> as_int = payload.FindInt("actionTimeoutMs");
  std::optional<double> as_double = payload.FindDouble("actionTimeoutMs");
  if (as_int.has_value()) {
    session.set_action_timeout(base::Milliseconds(*as_int));
  } else if (as_double.has_value()) {
    session.set_action_timeout(
        base::Milliseconds(static_cast<int>(*as_double)));
  }

  as_int = payload.FindInt("stepTimeoutMs");
  as_double = payload.FindDouble("stepTimeoutMs");
  if (as_int.has_value()) {
    session.set_step_timeout(base::Milliseconds(*as_int));
  } else if (as_double.has_value()) {
    session.set_step_timeout(
        base::Milliseconds(static_cast<int>(*as_double)));
  }

  base::DictValue result;
  result.Set("ok", true);
  // Echo the applied value (after clamp). Lets JS confirm what the
  // host actually set without an extra round trip.
  result.Set("appliedActionTimeoutMs",
             static_cast<int>(session.action_timeout().InMilliseconds()));
  result.Set("appliedStepTimeoutMs",
             static_cast<int>(session.step_timeout().InMilliseconds()));
  SendSuccessResponse(id, route, base::Value(std::move(result)));
}

void ToolsAdapter::HandleEndSession(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  std::string session_key = ExtractSessionKey(payload);

  std::string reason = "user_stopped";
  if (const std::string* r = payload.FindString("reason"); r && !r->empty()) {
    reason = *r;
  }

  // Was the session actually live before this call? Useful for the JS
  // caller to know whether anything was stopped (the JS surface
  // surfaces this as `{ ok, stopped }`, where `stopped` is `false` for
  // the second call when the session was already torn down).
  bool was_live = sessions_.find(session_key) != sessions_.end();
  OnSessionStopped(session_key, reason);

  base::DictValue result;
  result.Set("ok", true);
  result.Set("stopped", was_live);
  SendSuccessResponse(id, route, base::Value(std::move(result)));
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

          // Record the dispatch so the topbar's `nexusAgentStateChanged`
          // listener can flip to `running : <tool_name>` and bump the
          // total counter. The matching decrement (and success bump)
          // happens in ForwardSuccess / ForwardError, which run when the
          // tool's `ToolCallback` resolves.
          adapter->OnToolDispatchStarted(id, tool_name);

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
