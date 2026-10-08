#include "custom_browser/browser/nexus/api/session/nexus_session_bridge_adapter.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/tools/tool_session_registry.h"

namespace nexus {

namespace {

using Dict = base::DictValue;

// Pull the session id out of an `OnStop` / `SetAgentLoopSettings`
// payload. Two accepted shapes:
//   * preferred (what the JS guest's `buildHostedToolExecutionContext`
//     produces): `{ executionContext: { sessionId: "..." } }`
//   * legacy: top-level `{ sessionId: "..." }`
//
}  // namespace

NexusSessionBridgeAdapter::NexusSessionBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile,
    ToolSessionRegistry* sessions)
    : ::nexus::generated::SessionApiBase(delegate),
      profile_(profile),
      sessions_(sessions) {
  // Register(...) calls live in `SessionApiBase`'s ctor, generated from
  // api/declarations/session/*.api.json. Adding a new session wire
  // action: drop a *.api.json under api/declarations/session/, add a
  // Handle*OnUI() override here. The codegen wires the dispatch
  // automatically.

  // Observe the unified agent-config pref so settings changes are pushed
  // live to the guest. The fixed transport (`session.applyAgentConfig`
  // carrying the whole dict) never changes as settings are added.
  if (profile_ && profile_->GetPrefs()) {
    pref_change_registrar_.Init(profile_->GetPrefs());
    pref_change_registrar_.Add(
        prefs::kNexusAgentConfig,
        base::BindRepeating(&NexusSessionBridgeAdapter::PushAgentConfig,
                            base::Unretained(this)));
  }

  // Observe per-session tab-count changes (lazily creating the profile's
  // tracker) and fan them to the guest as `browser.sessionTabsChanged` —
  // the live feed behind the automatic webview_area visibility.
  if (NexusSessionTabTracker* tracker =
          NexusSessionTabTrackerFactory::GetForProfile(profile_)) {
    tab_tracker_observation_.Observe(tracker);
  }
}

NexusSessionBridgeAdapter::~NexusSessionBridgeAdapter() = default;

void NexusSessionBridgeAdapter::PushAgentConfig() {
  // Mirror the TS-side `session.applyAgentConfig` event envelope; the
  // transport wraps this as {hostBridge:true, direction:"event", ...}.
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "session");
  route.Set("action", "applyAgentConfig");

  Dict payload;
  payload.Set("config", GetAgentConfig(profile_));
  SendEvent(route, base::Value(std::move(payload)));
}

void NexusSessionBridgeAdapter::HandleOnStopOnUI(
    ::nexus::generated::OnStopInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!sessions_) {
    std::move(done).Error("SESSION_ERROR", "tool sessions unavailable");
    return;
  }

  // The frame context (`context.sessionId`), else the legacy payload fields.
  const std::string session_key = RequestSessionKey();

  // Recognized reason values are enumerated in
  // `custom_browser/browser/nexus/tools/tool_base.h::OnStopReason`.
  // `ParseOnStopReason` falls back to kUserStopped for empty /
  // unrecognized strings (the JS guest historically allowed missing
  // reason; the safe default preserves that contract).
  //
  // `kSessionDeleted` is the only reason that erases the ToolSession
  // from `ToolSessionRegistry::sessions_` and (consequently) destroys the
  // BrowserTool, which closes the session's tagged tabs. The JS guest
  // must pass that exact string when wiring sidebar delete; every
  // other reason leaves the ToolSession warm so the next turn reuses
  // it.
  const std::string_view reason_view =
      input.reason.has_value() ? std::string_view(*input.reason)
                                : std::string_view();
  const OnStopReason reason =
      ParseOnStopReason(reason_view, OnStopReason::kUserStopped);

  // Was the session live before this call? Useful for the JS caller to
  // distinguish "actually stopped something" from "no-op (session
  // already absent)" — surfaced as `{ ok, stopped }`.
  const bool was_live = sessions_->HasSession(session_key);
  sessions_->OnSessionStopped(session_key, reason);

  Dict result;
  result.Set("ok", true);
  result.Set("stopped", was_live);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSessionBridgeAdapter::HandleSetAgentLoopSettingsOnUI(
    ::nexus::generated::SetAgentLoopSettingsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!sessions_) {
    std::move(done).Error("SESSION_ERROR", "tool sessions unavailable");
    return;
  }

  // The frame context (`context.sessionId`), else the legacy payload fields.
  const std::string session_key = RequestSessionKey();

  // Lazily create the session so JS can push settings before the first
  // tool call. The OnStop cascade still cleans this up if the session
  // is later torn down without ever running a tool.
  ToolSession& session = sessions_->GetOrCreateSession(session_key);

  if (input.actionTimeoutMs.has_value()) {
    session.set_action_timeout(base::Milliseconds(*input.actionTimeoutMs));
  }
  if (input.stepTimeoutMs.has_value()) {
    session.set_step_timeout(base::Milliseconds(*input.stepTimeoutMs));
  }
  // ToolSession::set_cwd silently rejects relative paths, so empty / non-
  // absolute values don't clobber a previously-set cwd.
  if (input.cwd.has_value() && !input.cwd->empty()) {
    session.set_cwd(base::FilePath::FromUTF8Unsafe(*input.cwd));
  }
  if (input.clearReadState.value_or(false)) {
    session.file_tool().ClearReadState();
  }
  // T0-c browser observation short-circuit. Opt-in is the new-guest
  // handshake (old guests never send it → the feature stays off and
  // every observation is a full payload). The clear-cache flag marks
  // the last observation stale so the next one is re-sent full.
  if (input.browserObservationDiffOptIn.has_value()) {
    session.browser_tool().SetObservationDiffEnabled(
        *input.browserObservationDiffOptIn);
  }
  if (input.clearBrowserObservationCache.value_or(false)) {
    session.browser_tool().MarkObservationStale();
  }

  Dict result;
  result.Set("ok", true);
  // Echo applied values (after clamp / validation) so the JS caller can
  // confirm what the host actually set without an extra round trip.
  result.Set("appliedActionTimeoutMs",
             static_cast<int>(session.action_timeout().InMilliseconds()));
  result.Set("appliedStepTimeoutMs",
             static_cast<int>(session.step_timeout().InMilliseconds()));
  result.Set("appliedCwd", session.cwd().AsUTF8Unsafe());
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSessionBridgeAdapter::HandleSetActiveSessionOnUI(
    ::nexus::generated::SetActiveSessionInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Deliberately routed through the profile-keyed tracker, NOT
  // sessions_: this action arrives on the page port while the
  // session's BrowserTool (and its tab state) lives on the worker port's
  // ToolSessionRegistry. The tracker is the only cross-port view of the tab set.
  // Never creates a ToolSession — switching to a fresh chat session must
  // not allocate tool state.
  NexusSessionTabTracker* tracker =
      NexusSessionTabTrackerFactory::GetForProfile(profile_);
  if (!tracker) {
    std::move(done).Error("SESSION_ERROR", "tab tracker unavailable");
    return;
  }

  const bool activated = tracker->ActivateSessionTab(input.sessionId);
  const SessionTabCounts counts = tracker->GetCounts(input.sessionId);

  Dict result;
  result.Set("ok", true);
  result.Set("activated", activated);
  result.Set("nexusTabCount", counts.nexus_count);
  result.Set("totalTabCount", counts.total_count);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSessionBridgeAdapter::OnSessionTabCountsChanged(
    const std::string& session_key,
    const SessionTabCounts& counts) {
  // Event envelope mirrors PushAgentConfig: the transport wraps this as
  // {hostBridge:true, direction:"event", ...}. The guest's
  // `subscribeSessionTabsChanged` (nexusBridgeActions.ts) filters on the
  // route; sessions it doesn't know (e.g. "__default__") are ignored there.
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "browser");
  route.Set("action", "sessionTabsChanged");

  Dict payload;
  payload.Set("sessionId", session_key);
  payload.Set("nexusTabCount", counts.nexus_count);
  payload.Set("totalTabCount", counts.total_count);
  SendEvent(route, base::Value(std::move(payload)));
}

}  // namespace nexus
