#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "chrome/common/actor.mojom.h"
#include "components/sessions/core/session_id.h"

namespace nexus::browser_tool_internal {

// =============================================================================
// PausableTimeout — out-of-line ctor / dtor / methods. Chromium-style
// requires complex bodies (any class with non-trivial members) to be
// defined in the .cc, even if the body is `= default;`.
// =============================================================================

PausableTimeout::PausableTimeout(base::TimeDelta budget,
                                 std::string name,
                                 base::OnceClosure on_timeout)
    : remaining_(budget),
      name_(std::move(name)),
      on_timeout_(std::move(on_timeout)) {}

PausableTimeout::~PausableTimeout() = default;

void PausableTimeout::Start(bool paused) {
  if (!remaining_.is_positive()) {
    Stop();
    return;
  }
  paused_ = paused;
  if (!paused_) {
    ArmTimer();
  }
}

void PausableTimeout::Pause() {
  if (settled_ || paused_) {
    return;
  }
  if (timer_.IsRunning()) {
    remaining_ -= base::TimeTicks::Now() - started_at_;
    timer_.Stop();
  }
  paused_ = true;
}

void PausableTimeout::Resume() {
  if (settled_ || !paused_) {
    return;
  }
  paused_ = false;
  if (!remaining_.is_positive()) {
    Fire();
    return;
  }
  ArmTimer();
}

void PausableTimeout::Stop() {
  if (settled_) {
    return;
  }
  settled_ = true;
  timer_.Stop();
  on_timeout_.Reset();
}

void PausableTimeout::ArmTimer() {
  started_at_ = base::TimeTicks::Now();
  timer_.Start(FROM_HERE, remaining_, this, &PausableTimeout::Fire);
}

void PausableTimeout::Fire() {
  scoped_refptr<PausableTimeout> keep_alive(this);
  if (settled_) {
    return;
  }
  settled_ = true;
  timer_.Stop();
  LOG(WARNING) << "[BrowserTool] " << name_ << " exceeded timeout";
  if (on_timeout_) {
    std::move(on_timeout_).Run();
  }
}

// =============================================================================
// PausableToolCallbackHolder — same out-of-line treatment.
// =============================================================================

PausableToolCallbackHolder::PausableToolCallbackHolder(
    ::nexus::ToolCallback inner,
    std::string action_name)
    : inner_(std::move(inner)), action_name_(std::move(action_name)) {}

PausableToolCallbackHolder::~PausableToolCallbackHolder() = default;

void PausableToolCallbackHolder::SetTimeout(
    scoped_refptr<PausableTimeout> timeout) {
  timeout_ = std::move(timeout);
}

void PausableToolCallbackHolder::Resolve(base::Value value) {
  if (timeout_) {
    timeout_->Stop();
  }
  if (inner_.fired()) {
    return;
  }
  std::move(inner_).Resolve(std::move(value));
}

void PausableToolCallbackHolder::Reject(std::string code,
                                        std::string message) {
  if (timeout_) {
    timeout_->Stop();
  }
  if (inner_.fired()) {
    return;
  }
  std::move(inner_).Reject(std::move(code), std::move(message));
}

void PausableToolCallbackHolder::OnTimeout() {
  if (inner_.fired()) {
    return;
  }
  std::move(inner_).Reject(
      "tool_action_timeout",
      base::StrCat({action_name_,
                    ": exceeded per-action wall-clock budget"}));
}

const actor::EnterprisePolicyUrlChecker* GetPolicyChecker(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto* service =
      glic::GlicKeyedServiceFactory::GetGlicKeyedService(profile);
  if (!service) {
    return nullptr;
  }
  return &service->actor_policy_checker();
}

std::string DescribeActorError(actor::mojom::ActionResultCode code) {
  const int n = static_cast<int>(code);
  std::string_view desc;
  switch (code) {
    case actor::mojom::ActionResultCode::kUrlBlocked:
      desc = "actions are not allowed on the current URL";
      break;
    case actor::mojom::ActionResultCode::kNewTabCreationFailed:
      desc = "could not open a new tab";
      break;
    case actor::mojom::ActionResultCode::kTabWentAway:
      desc = "the target tab no longer exists";
      break;
    case actor::mojom::ActionResultCode::kTaskWentAway:
      desc = "the actor task for this action no longer exists";
      break;
    case actor::mojom::ActionResultCode::kCrossOriginNavigation:
      desc =
          "the tab performed a cross-origin navigation and can't be acted "
          "on; re-observe before retrying";
      break;
    case actor::mojom::ActionResultCode::kToolUnknown:
      desc = "unknown tool requested";
      break;
    case actor::mojom::ActionResultCode::kFrameWentAway:
      desc = "the target frame is gone or shutting down";
      break;
    case actor::mojom::ActionResultCode::kInvalidDomNodeId:
      desc =
          "the target index is no longer in the DOM (stale index — "
          "re-observe and use indexes from the latest observation)";
      break;
    case actor::mojom::ActionResultCode::kElementDisabled:
      desc = "the target element is disabled";
      break;
    case actor::mojom::ActionResultCode::kElementOffscreen:
      desc =
          "the element is offscreen — prepend a scroll_to action "
          "targeting that index, then retry";
      break;
    case actor::mojom::ActionResultCode::kCoordinatesOutOfBounds:
      desc = "the supplied coordinate is outside the viewport";
      break;
    case actor::mojom::ActionResultCode::kArgumentsInvalid:
      desc = "the action's arguments are invalid for this target";
      break;
    case actor::mojom::ActionResultCode::kTaskPaused:
      desc = "the actor task was paused";
      break;
    case actor::mojom::ActionResultCode::kExecutorDestroyed:
      desc =
          "the tool executor was destroyed before the action finished — "
          "the page may have navigated";
      break;
    case actor::mojom::ActionResultCode::kWindowWentAway:
      desc = "the target window no longer exists";
      break;
    case actor::mojom::ActionResultCode::kFrameLocationChangedSinceObservation:
      desc =
          "the page changed location since the observation — re-observe "
          "before retrying";
      break;
    case actor::mojom::ActionResultCode::kTriggeredNavigationBlocked:
      desc = "the action triggered a navigation that was blocked";
      break;
    case actor::mojom::ActionResultCode::kEmptyActionSequence:
      desc = "the actor was given no actions to run";
      break;
    case actor::mojom::ActionResultCode::kExecutorBusy:
      desc =
          "the tool executor is busy with another action — wait briefly "
          "and retry";
      break;
    case actor::mojom::ActionResultCode::kObservedTargetElementDestroyed:
      desc =
          "the target element was destroyed between observation and "
          "action — re-observe before retrying";
      break;
    case actor::mojom::ActionResultCode::kObservedTargetElementChanged:
      desc =
          "the target element changed between observation and action — "
          "re-observe before retrying";
      break;
    case actor::mojom::ActionResultCode::kTargetNodeInteractionPointObscured:
      desc =
          "the target's interaction point is obscured by another element "
          "— try a different element (e.g. find_text on the visible "
          "label and click the clickable_ancestor_index)";
      break;
    case actor::mojom::ActionResultCode::kToolTimeout:
      desc =
          "the action took too long to execute — try a smaller batch or "
          "a different element";
      break;
    case actor::mojom::ActionResultCode::kFilePickerTriggered:
      desc =
          "the action triggered a file picker — file uploads aren't "
          "automatable; ask the user to handle this step";
      break;
    case actor::mojom::ActionResultCode::kFilePickerConfirmed:
      desc = "a file picker was confirmed";
      break;
    case actor::mojom::ActionResultCode::kFilePickerCancelled:
      desc = "the file picker was cancelled";
      break;
    case actor::mojom::ActionResultCode::kExecutionEngineExistingAction:
      desc =
          "the execution engine already has an action sequence in flight "
          "(typically from a prior bridge_timeout that left a stale "
          "task) — wait briefly, or close and reopen the tab to recover";
      break;
    case actor::mojom::ActionResultCode::kExternalProtocolNavigationBlocked:
      desc = "navigation to an external protocol was blocked";
      break;
    case actor::mojom::ActionResultCode::kRendererCrashed:
      desc = "the renderer process crashed; the page is unusable";
      break;
    case actor::mojom::ActionResultCode::kNotImplemented:
      desc = "this action is not yet implemented";
      break;
    case actor::mojom::ActionResultCode::kInvokeCanceled:
      desc = "the action was canceled before it ran";
      break;
    case actor::mojom::ActionResultCode::kInvalidTaskStateForAct:
      desc = "the actor task is in an invalid state for this action";
      break;
    case actor::mojom::ActionResultCode::kActionsCancelled:
      desc = "the remaining actions in this batch were cancelled";
      break;
    case actor::mojom::ActionResultCode::kNavigateInvalidUrl:
      desc = "navigate: the URL is not valid";
      break;
    case actor::mojom::ActionResultCode::kNavigateFailedToStart:
      desc =
          "navigate: the navigation failed before it started "
          "(network/DNS/blocked)";
      break;
    case actor::mojom::ActionResultCode::kNavigateCommittedErrorPage:
      desc =
          "navigate: landed on an error page (404 / 500 / cert error) — "
          "report and don't retry blindly";
      break;
    case actor::mojom::ActionResultCode::kClickSuppressed:
      desc = "click: the click event was suppressed by the page";
      break;
    default:
      // Unknown code — fall back to the bare numeric form so the LLM
      // still has a stable identifier and humans can map it via the
      // mojom enum.
      return base::StrCat({"actor_error_", base::NumberToString(n)});
  }
  return base::StrCat({"actor_error_", base::NumberToString(n), ": ", desc});
}

actor::ActorTask::StoppedReason MapStopReason(const std::string& reason) {
  if (reason == "user_stopped") {
    return actor::ActorTask::StoppedReason::kStoppedByUser;
  }
  if (reason == "completed") {
    return actor::ActorTask::StoppedReason::kTaskComplete;
  }
  if (reason == "disconnected" || reason == "shutdown" ||
      reason == "destructor") {
    return actor::ActorTask::StoppedReason::kShutdown;
  }
  return actor::ActorTask::StoppedReason::kStoppedByUser;
}

Browser* GetActiveBrowserForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return chrome::FindLastActiveWithProfile(profile);
}

std::optional<int32_t> GetActiveWindowSessionId(Profile* profile) {
  Browser* browser = GetActiveBrowserForProfile(profile);
  if (!browser) {
    return std::nullopt;
  }
  return browser->session_id().id();
}

std::optional<int32_t> GetActiveTabHandleId(Profile* profile) {
  Browser* browser = GetActiveBrowserForProfile(profile);
  if (!browser) {
    return std::nullopt;
  }
  TabStripModel* tab_strip = browser->tab_strip_model();
  if (!tab_strip) {
    return std::nullopt;
  }
  tabs::TabInterface* active = tab_strip->GetActiveTab();
  if (!active) {
    return std::nullopt;
  }
  return active->GetHandle().raw_value();
}

std::optional<int32_t> ResolveTabIdOrReject(
    std::optional<int> explicit_tab_id,
    Profile* profile,
    std::string_view tool_name,
    ::nexus::ToolCallback& done) {
  if (explicit_tab_id.has_value()) {
    return *explicit_tab_id;
  }
  std::optional<int32_t> active = GetActiveTabHandleId(profile);
  if (!active.has_value()) {
    std::move(done).Reject(
        "no_active_tab",
        base::StrCat({tool_name,
                      ": no active tab; pass `tab_id` explicitly"}));
    return std::nullopt;
  }
  return *active;
}

void MaybePrependActivateTab(apc::Actions* actions,
                             int32_t target_tab_id,
                             Profile* profile) {
  std::optional<int32_t> active = GetActiveTabHandleId(profile);
  if (!active.has_value() || *active == target_tab_id) {
    return;
  }
  apc::ActivateTabAction* activate =
      actions->add_actions()->mutable_activate_tab();
  activate->set_tab_id(target_tab_id);
}

const char* ContentAttributeTypeName(apc::ContentAttributeType t) {
  switch (t) {
    case apc::CONTENT_ATTRIBUTE_ANCHOR:
      return "link";
    case apc::CONTENT_ATTRIBUTE_FORM_CONTROL:
      return "form_control";
    case apc::CONTENT_ATTRIBUTE_FORM:
      return "form";
    case apc::CONTENT_ATTRIBUTE_HEADING:
      return "heading";
    case apc::CONTENT_ATTRIBUTE_PARAGRAPH:
      return "paragraph";
    case apc::CONTENT_ATTRIBUTE_TEXT:
      return "text";
    case apc::CONTENT_ATTRIBUTE_IMAGE:
      return "image";
    case apc::CONTENT_ATTRIBUTE_TABLE:
      return "table";
    case apc::CONTENT_ATTRIBUTE_LIST_ITEM:
      return "list_item";
    case apc::CONTENT_ATTRIBUTE_IFRAME:
      return "iframe";
    case apc::CONTENT_ATTRIBUTE_VIDEO:
      return "video";
    default:
      return "element";
  }
}

std::string ExtractNodeText(const apc::ContentAttributes& attrs) {
  if (attrs.has_label() && !attrs.label().empty()) {
    return attrs.label();
  }
  if (attrs.has_text_data() && !attrs.text_data().text_content().empty()) {
    return attrs.text_data().text_content();
  }
  return std::string();
}

bool IsActionable(const apc::ContentAttributes& attrs) {
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR ||
      attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL) {
    return true;
  }
  if (attrs.has_interaction_info()) {
    const auto& ii = attrs.interaction_info();
    if (ii.is_clickable() || ii.is_focusable()) {
      return true;
    }
  }
  return false;
}

std::string TruncateUtf8Snippet(std::string s, int max_bytes) {
  if (static_cast<int>(s.size()) <= max_bytes) {
    return s;
  }
  s.resize(static_cast<size_t>(max_bytes));
  size_t i = s.size();
  while (i > 0) {
    --i;
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if ((b & 0x80) == 0) {
      // Single-byte ASCII: complete, keep [0, i+1].
      s.resize(i + 1);
      return s;
    }
    if ((b & 0xC0) == 0xC0) {
      // Leading byte of a multi-byte sequence. Compute its expected
      // length and drop the partial sequence if `s.size()-i` is short.
      const size_t expected = (b & 0xE0) == 0xC0   ? 2
                              : (b & 0xF0) == 0xE0 ? 3
                              : (b & 0xF8) == 0xF0 ? 4
                                                   : 1;
      if (s.size() - i >= expected) {
        return s;
      }
      s.resize(i);
      return s;
    }
    // Continuation byte (0b10xxxxxx) — keep walking back.
  }
  s.clear();
  return s;
}

}  // namespace nexus::browser_tool_internal
