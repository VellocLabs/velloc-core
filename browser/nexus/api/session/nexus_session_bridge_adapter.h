#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SESSION_NEXUS_SESSION_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SESSION_NEXUS_SESSION_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/session/session_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

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
// All three actions run on UI (the bridge's home thread) — they touch
// browser-process state (LlmBroker via Profile keyed services,
// ToolsAdapter's per-session map) which is UI-bound.
//
// The adapter holds a back-reference to `ToolsAdapter` because two
// of its actions need to reach into the session map. The lifetime
// invariant is set by `NexusBridgePortService`: both adapters are
// owned by the same port service and destroyed together, so the raw
// pointer is safe for the adapter's lifetime.
class NexusSessionBridgeAdapter
    : public ::nexus::generated::SessionApiBase {
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

  raw_ptr<Profile> profile_;
  raw_ptr<ToolsAdapter> tools_adapter_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SESSION_NEXUS_SESSION_BRIDGE_ADAPTER_H_
