#include "custom_browser/browser/nexus/api/session/nexus_session_bridge_adapter.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/tools/tools_adapter.h"

namespace nexus {

namespace {

using Dict = base::DictValue;

// Pull the session id out of an `OnStop` / `SetAgentLoopSettings`
// payload. Two accepted shapes:
//   * preferred (what the JS guest's `buildHostedToolExecutionContext`
//     produces): `{ executionContext: { sessionId: "..." } }`
//   * legacy: top-level `{ sessionId: "..." }`
//
// A missing or empty session id falls through to the "__default__"
// sentinel so single-session callers don't have to thread the field
// every call. Multi-session callers always populate it. Mirrors the
// file-local `ExtractSessionKey` in `tools/tools_adapter.cc` — kept
// duplicated rather than exported because (a) the two callers have
// slightly different input shapes (typed struct vs raw dict) and (b)
// the helper is small enough that pulling it into a header isn't
// worth the additional include.
constexpr char kDefaultSessionKey[] = "__default__";

std::string SessionKeyFromExecutionContext(
    const std::optional<base::DictValue>& execution_context,
    const std::optional<std::string>& legacy_session_id) {
  if (execution_context.has_value()) {
    if (const std::string* sid =
            execution_context->FindString("sessionId");
        sid && !sid->empty()) {
      return *sid;
    }
  }
  if (legacy_session_id.has_value() && !legacy_session_id->empty()) {
    return *legacy_session_id;
  }
  return kDefaultSessionKey;
}

}  // namespace

NexusSessionBridgeAdapter::NexusSessionBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile,
    ToolsAdapter* tools_adapter)
    : ::nexus::generated::SessionApiBase(delegate),
      profile_(profile),
      tools_adapter_(tools_adapter) {
  // Register(...) calls live in `SessionApiBase`'s ctor, generated from
  // api/declarations/session/*.api.json. Adding a new session wire
  // action: drop a *.api.json under api/declarations/session/, add a
  // Handle*OnUI() override here. The codegen wires the dispatch
  // automatically.
}

NexusSessionBridgeAdapter::~NexusSessionBridgeAdapter() = default;

void NexusSessionBridgeAdapter::HandlePublishStatusOnUI(
    ::nexus::generated::PublishStatusInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Payload shape: SessionStatus directly. Stored verbatim so future
  // extensions don't require adapter changes — pre-Phase-12f the same
  // logic lived in `NexusLlmBridgeAdapter::HandlePublishStatusOnUI`.
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "LlmBroker unavailable");
    return;
  }
  broker->SetSessionStatus(std::move(input.payload));
  std::move(done).SuccessVoid();
}

void NexusSessionBridgeAdapter::HandleOnStopOnUI(
    ::nexus::generated::OnStopInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!tools_adapter_) {
    std::move(done).Error("SESSION_ERROR", "ToolsAdapter unavailable");
    return;
  }

  const std::string session_key =
      SessionKeyFromExecutionContext(input.executionContext, input.sessionId);

  std::string reason = "user_stopped";
  if (input.reason.has_value() && !input.reason->empty()) {
    reason = *input.reason;
  }

  // Was the session live before this call? Useful for the JS caller to
  // distinguish "actually stopped something" from "no-op (session
  // already absent)" — surfaced as `{ ok, stopped }`.
  const bool was_live = tools_adapter_->HasSession(session_key);
  tools_adapter_->OnSessionStopped(session_key, reason);

  Dict result;
  result.Set("ok", true);
  result.Set("stopped", was_live);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSessionBridgeAdapter::HandleSetAgentLoopSettingsOnUI(
    ::nexus::generated::SetAgentLoopSettingsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!tools_adapter_) {
    std::move(done).Error("SESSION_ERROR", "ToolsAdapter unavailable");
    return;
  }

  const std::string session_key =
      SessionKeyFromExecutionContext(input.executionContext, input.sessionId);

  // Lazily create the session so JS can push settings before the first
  // tool call. The OnStop cascade still cleans this up if the session
  // is later torn down without ever running a tool.
  ToolSession& session = tools_adapter_->GetOrCreateSession(session_key);

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

}  // namespace nexus
