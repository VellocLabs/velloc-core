#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SESSION_NEXUS_SESSION_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SESSION_NEXUS_SESSION_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "components/prefs/pref_change_registrar.h"
#include "custom_browser/browser/nexus/api/session/session_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tracker.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;
class ToolsAdapter;

// Concrete adapter implementing the generated `SessionApiBase` virtuals.
//
// Phase 12f architecture: the `session` capability used to be a wire-
// level alias for a slice of LLM-adapter functionality (`publishStatus`
// was registered under `api/declarations/llm/` and dispatched through
// `NexusLlmBridgeAdapter`) PLUS a pair of manual handlers in
// `ToolsAdapter` (`browser.endSession`, `browser.setAgentLoopSettings`).
//
// This adapter consolidates all three under one `session` capability
// served by a dedicated codegen base class:
//
//   - `publishStatus` — moved from the LLM declarations folder. The
//     payload (a `SessionStatus` dict) is forwarded verbatim to
//     `LlmBroker::SetSessionStatus`. The broker still lives in
//     `api/llm/`; only the bridge entrypoint moved.
//
//   - `onStop` — renamed from the manual `browser.endSession` handler.
//     Cascades through `ToolsAdapter::OnSessionStopped` →
//     `ToolSession::OnStop(reason)` → each per-category OnStop. The
//     JS-side stop hook in nexus_web fires this with
//     `reason: 'completed'` on conversation completion; the Stop
//     button fires with `reason: 'user_stopped'`. Idempotent.
//
//   - `setAgentLoopSettings` — moved from the manual
//     `browser.setAgentLoopSettings` handler. Pushes per-session
//     wall-clock budgets (`actionTimeoutMs`, `stepTimeoutMs`) and
//     default cwd onto the matching `ToolSession`. Lazily creates
//     the session if absent so JS can push settings before the first
//     tool call.
//
//   - `setActiveSession` — guest reports the active chat session on boot
//     and on every switch. Activates one of the session's kNexus-window
//     tabs (via the profile-keyed `NexusSessionTabTracker` — NOT this
//     port's ToolsAdapter, whose BrowserTool state belongs to a
//     different port) and resolves with the session's tab counts.
//
// All actions run on UI (the bridge's home thread) — they touch
// browser-process state (LlmBroker via Profile keyed services,
// ToolsAdapter's per-session map) which is UI-bound.
//
// The adapter holds a back-reference to `ToolsAdapter` because two
// of its actions need to reach into the session map. The lifetime
// invariant is set by `NexusBridgePortService`: both adapters are
// owned by the same port service and destroyed together, so the raw
// pointer is safe for the adapter's lifetime.
//
// The adapter also observes `NexusSessionTabTracker` and forwards every
// per-session count change to the guest as a `browser.sessionTabsChanged`
// event — the live feed behind the guest's automatic webview_area
// visibility. Every port's adapter observes (page + worker), so the event
// goes out on both ports; only the page port has a guest-side subscriber,
// the duplicate is inert (same shape as `session.applyAgentConfig`).
class NexusSessionBridgeAdapter
    : public ::nexus::generated::SessionApiBase,
      public NexusSessionTabTracker::Observer {
 public:
  NexusSessionBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            Profile* profile,
                            ToolsAdapter* tools_adapter);
  ~NexusSessionBridgeAdapter() override;

  NexusSessionBridgeAdapter(const NexusSessionBridgeAdapter&) = delete;
  NexusSessionBridgeAdapter& operator=(const NexusSessionBridgeAdapter&) =
      delete;

 private:
  // Overrides of generated::SessionApiBase pure virtuals.

  void HandlePublishStatusOnUI(
      ::nexus::generated::PublishStatusInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleOnStopOnUI(
      ::nexus::generated::OnStopInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetAgentLoopSettingsOnUI(
      ::nexus::generated::SetAgentLoopSettingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetActiveSessionOnUI(
      ::nexus::generated::SetActiveSessionInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // NexusSessionTabTracker::Observer — forwarded to the guest verbatim as
  // a `browser.sessionTabsChanged` event.
  void OnSessionTabCountsChanged(const std::string& session_key,
                                 const SessionTabCounts& counts) override;

  // Pushes the unified agent runtime config to the guest as a
  // `session.applyAgentConfig` event. Invoked whenever the
  // `nexus.agent_config` pref changes. Post-Path-2 the ONLY remaining
  // writer of this pref is `LlmBroker::SetDefaultSelection` — when the
  // user picks a model in the chromium topbar, the broker mirrors the
  // selection into `agent_config.model` so this observer fires and
  // delivers the new model to the JS guest's `MultiSessionShell.tsx`
  // subscriber. Loop limits (maxTurns / stepTimeoutSec) and tracer
  // flags (log / trace) NO LONGER ride this channel — they live in
  // `~/.nexus/settings.json` `agent` and broadcast via the JS-owned
  // `agent.changed` WebUI bridge event. The boot snapshot for either
  // surface is delivered separately via the bootstrap manifest +
  // session-boot overlay respectively.
  void PushAgentConfig();

  raw_ptr<Profile> profile_;
  raw_ptr<ToolsAdapter> tools_adapter_;

  // Observes `nexus.agent_config`; on change → PushAgentConfig().
  // See PushAgentConfig() for the post-Path-2 scope reduction.
  PrefChangeRegistrar pref_change_registrar_;

  // Observes per-session tab-count changes; see class comment.
  base::ScopedObservation<NexusSessionTabTracker,
                          NexusSessionTabTracker::Observer>
      tab_tracker_observation_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SESSION_NEXUS_SESSION_BRIDGE_ADAPTER_H_
