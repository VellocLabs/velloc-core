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
class ToolSessionRegistry;

// Concrete adapter implementing the generated `SessionApiBase` virtuals:
// `onStop` (idempotent, cascades to each per-category OnStop),
// `setAgentLoopSettings` (per-session budgets and cwd; creates the session)
// and `setActiveSession` (activates a session tab via the profile-keyed
// `NexusSessionTabTracker`, returns tab counts). UI thread. The raw
// `ToolSessionRegistry` pointer is safe: `NexusBridgePortService` owns both.
// Tracker count changes go out as `browser.sessionTabsChanged`.
class NexusSessionBridgeAdapter
    : public ::nexus::generated::SessionApiBase,
      public NexusSessionTabTracker::Observer {
 public:
  NexusSessionBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            Profile* profile,
                            ToolSessionRegistry* sessions);
  ~NexusSessionBridgeAdapter() override;

  NexusSessionBridgeAdapter(const NexusSessionBridgeAdapter&) = delete;
  NexusSessionBridgeAdapter& operator=(const NexusSessionBridgeAdapter&) =
      delete;

 private:
  // Overrides of generated::SessionApiBase pure virtuals.

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

  // Pushes `nexus.agent_config` to the guest as `session.applyAgentConfig`
  // whenever the pref changes; its only writer is LlmBroker::SetDefaultSelection.
  void PushAgentConfig();

  raw_ptr<Profile> profile_;
  raw_ptr<ToolSessionRegistry> sessions_;

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
