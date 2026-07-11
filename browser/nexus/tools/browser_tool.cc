#include "custom_browser/browser/nexus/tools/browser_tool.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor_webui.mojom.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"

namespace nexus {

namespace {

namespace apc = optimization_guide::proto;

// Pulls the production EnterprisePolicyUrlChecker out of GlicKeyedService.
// Same indirection the deleted NexusGlicBridgeAdapter used — the upstream
// chrome://glic infrastructure is still where the policy checker lives,
// so we reach for it via the same factory rather than reimplementing.
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

// Map ActionResultCode → "actor_error_<N>: <human description>". The
// description text mirrors the comments in chrome/common/actor.mojom
// (LINT.IfChange(ActionResultCode) block). The bare numeric form is
// preserved as a prefix so existing log greps and code-based handling
// still match — humans (and the LLM) just get the explanation appended.
//
// We intentionally don't try to enumerate every code; unknown values
// fall through to the bare numeric form. The codes covered here are
// the ones that show up in practice and that the LLM can act on
// (re-observe, scroll, give up, etc.). Extend on demand.
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

// Maps the framework's `OnStop(reason)` string to `actor::ActorTask::
// StoppedReason`. Used when the per-session `BrowserTool` is told to
// stop and needs to forward an actor-friendly reason to
// `actor::ActorKeyedService::StopTask`. The reason flows into UMA via
// `actor_metrics::RecordActorTaskCompletion`, so the mapping should
// match the user-facing semantic of each trigger.
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
  // Unknown / future reasons fall back to the generic user-stopped
  // bucket; the textual reason is still surfaced in the [BrowserTool]
  // log line above.
  return actor::ActorTask::StoppedReason::kStoppedByUser;
}

// =====================================================================
// Active-tab / active-window resolvers
// =====================================================================
//
// Most actor proto actions (NavigateAction, HistoryBackAction,
// CreateTabAction, …) reject the request unless the corresponding
// tab/window id is set explicitly — see `actor_proto_conversion.cc`'s
// `Create*Request` helpers, which return nullptr on
// `tab_handle == TabHandle::Null()` or `!has_window_id()`. The Nexus
// tools advertise `tab_id` as optional ("Optional. Active tab if
// absent.") because the LLM almost always wants to act on the
// foreground tab; these helpers materialize that "active tab"
// fallback by resolving against the most-recently-active Browser
// for the current Profile.

// Returns the most-recently-active `Browser*` whose Profile matches
// `profile`, or `nullptr` if none is open. Wraps
// `chrome::FindLastActiveWithProfile` with a profile-null guard so the
// callers don't need to.
Browser* GetActiveBrowserForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return chrome::FindLastActiveWithProfile(profile);
}

// Returns the SessionID::id_type (int32) of the active window for
// `profile`, or std::nullopt if no Browser exists yet. Used to populate
// `CreateTabAction.window_id`, which the actor framework requires.
std::optional<int32_t> GetActiveWindowSessionId(Profile* profile) {
  Browser* browser = GetActiveBrowserForProfile(profile);
  if (!browser) {
    return std::nullopt;
  }
  return browser->session_id().id();
}

// Returns the `tabs::TabHandle::raw_value()` of the active tab in the
// most-recently-active Browser for `profile`, or std::nullopt if no
// Browser/active tab exists. Used as the default `tab_id` for action
// protos whose `tab_id` was left unset by the LLM.
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

// Resolves the explicit-or-active tab_id for a tool method. Returns
// the value to pass to `set_tab_id(...)`. On miss (no tab_id given AND
// no active tab) rejects `done` with `no_active_tab` and returns
// std::nullopt — the caller MUST early-return.
std::optional<int32_t> ResolveTabIdOrReject(
    std::optional<int> explicit_tab_id,
    Profile* profile,
    std::string_view tool_name,
    ToolCallback& done) {
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

// If `target_tab_id` is not currently the active tab for `profile`,
// prepend an `ActivateTabAction` to `actions` so the actor framework
// foregrounds the target tab before the caller's action runs. Caller
// invokes this BEFORE adding its own action(s) to the proto, so the
// resulting sequence is [ActivateTab, <caller's action>].
//
// No-op when (a) the target tab is already active or (b) the active
// tab can't be resolved (single-action runner has nothing to compare
// against; the caller's action will fail downstream with the same
// no-active-tab condition the actor surfaces).
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

constexpr const char* kBrowserActionsDescription =
    "Submit browser actions for the current observed browser state. "
    "Use one clear goal per call. actions is an array of 1-5 action "
    "objects. Each action object has a simple type plus that action's "
    "parameters. Element-targeting actions use index, the element id "
    "from the latest observation. Page-changing actions should be "
    "last because remaining queued actions are skipped after "
    "URL/focus changes, action errors, done, or sequence "
    "terminators.\n\n"
    "Tab activation is automatic: when an action targets a tab_id "
    "that is not currently the foreground tab, that tab is activated "
    "first. create_tab opens the new tab in the foreground. You do not "
    "need to emit a separate activate_tab before acting on a tab.\n\n"
    "Action types preserve the old browser_* tool behavior with simpler "
    "names:\n"
    "- click: Click a page element. Params: index, optional tab_id, button "
    "(left/right), click_count (single/double).\n"
    "- input: Replace the contents of a form control with text. This "
    "overwrites any existing value; optional submit presses Enter after "
    "typing. Params: index, text, optional "
    "tab_id, submit, clear.\n"
    "- select: Pick an option from a <select>; value must match exactly. "
    "Params: index, value, optional tab_id.\n"
    "- hover: Move the cursor over an element without clicking it; use to "
    "reveal hover-only UI. Params: index, optional tab_id.\n"
    "- drag_and_drop: Press from_index, drag to to_index, and release; both "
    "elements must be on the same tab. Params: from_index, to_index, "
    "optional tab_id.\n"
    "- navigate: Navigate a tab to a direct URL. Do not use this to open a "
    "search homepage; use search for web queries. Params: url, optional "
    "tab_id.\n"
    "- scroll_to: Scroll an element into view. Params: index, optional "
    "tab_id.\n"
    "- scroll: Scroll the viewport, or scroll inside an element by setting "
    "index. Params: direction (up/down/left/right), distance (pixels — "
    "e.g. 400 for one viewport-page; values <=10 are treated as wheel "
    "notches and expanded ~120x), optional index, tab_id.\n"
    "- wait: Wait for page loads or animations. Params: optional tab_id, "
    "wait_ms.\n"
    "- back / forward: Move in tab history. Params: optional tab_id.\n"
    "- search: Preferred first step for requests to search/find/look up the "
    "web. Params: query, optional engine (google, bing, baidu, duckduckgo).\n"
    "- create_tab: Open a direct URL in a new tab. Params: url, optional "
    "reuse_visible_tab.\n"
    "- close_tab / activate_tab: Close or foreground a tab. Params: "
    "optional tab_id for close_tab, required tab_id for activate_tab.\n"
    "- media_play / media_pause / media_seek / media_state: Drive or read "
    "the page MediaSession. media_seek requires seek_time_ms.\n"
    "- screenshot: Observe with screenshot when visual context is needed. "
    "Params: optional tab_id.\n"
    "- read_page: Get visible page text. Params: optional tab_id, scope "
    "(viewport/full_page), max_chars.\n"
    "- read_element: Get text inside one element. Params: index, optional "
    "tab_id, max_chars.\n"
    "- find_text: Find visible text and resolve it to a clickable element. "
    "Params: query, optional tab_id, max_results.\n"
    "- done: Finish the task. Params: success, text.";

}  // namespace

class PausableTimeout : public base::RefCounted<PausableTimeout> {
 public:
  PausableTimeout(base::TimeDelta budget,
                  std::string name,
                  base::OnceClosure on_timeout)
      : remaining_(budget),
        name_(std::move(name)),
        on_timeout_(std::move(on_timeout)) {}

  void Start(bool paused) {
    if (!remaining_.is_positive()) {
      Stop();
      return;
    }
    paused_ = paused;
    if (!paused_) {
      ArmTimer();
    }
  }

  void Pause() {
    if (settled_ || paused_) {
      return;
    }
    if (timer_.IsRunning()) {
      remaining_ -= base::TimeTicks::Now() - started_at_;
      timer_.Stop();
    }
    paused_ = true;
  }

  void Resume() {
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

  void Stop() {
    if (settled_) {
      return;
    }
    settled_ = true;
    timer_.Stop();
    on_timeout_.Reset();
  }

  bool settled() const { return settled_; }

 private:
  friend class base::RefCounted<PausableTimeout>;
  ~PausableTimeout() = default;

  void ArmTimer() {
    started_at_ = base::TimeTicks::Now();
    timer_.Start(FROM_HERE, remaining_, this, &PausableTimeout::Fire);
  }

  void Fire() {
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

  base::TimeDelta remaining_;
  std::string name_;
  base::OnceClosure on_timeout_;
  base::OneShotTimer timer_;
  base::TimeTicks started_at_;
  bool paused_ = false;
  bool settled_ = false;
};

class PausableToolCallbackHolder
    : public base::RefCounted<PausableToolCallbackHolder> {
 public:
  PausableToolCallbackHolder(ToolCallback inner, std::string action_name)
      : inner_(std::move(inner)), action_name_(std::move(action_name)) {}

  void SetTimeout(scoped_refptr<PausableTimeout> timeout) {
    timeout_ = std::move(timeout);
  }

  void Resolve(base::Value value) {
    if (timeout_) {
      timeout_->Stop();
    }
    if (inner_.fired()) {
      return;
    }
    std::move(inner_).Resolve(std::move(value));
  }

  void Reject(std::string code, std::string message) {
    if (timeout_) {
      timeout_->Stop();
    }
    if (inner_.fired()) {
      return;
    }
    std::move(inner_).Reject(std::move(code), std::move(message));
  }

  void OnTimeout() {
    if (inner_.fired()) {
      return;
    }
    std::move(inner_).Reject(
        "tool_action_timeout",
        base::StrCat({action_name_,
                      ": exceeded per-action wall-clock budget"}));
  }

 private:
  friend class base::RefCounted<PausableToolCallbackHolder>;
  ~PausableToolCallbackHolder() = default;

  ToolCallback inner_;
  std::string action_name_;
  scoped_refptr<PausableTimeout> timeout_;
};

// =============================================================================
// Schema declarations — single source of truth. Co-located with the
// implementations below so adding/editing a tool means editing one .cc
// file. The static `ToolCatalog` calls this once at first access.
// =============================================================================

namespace {

// Hand-crafted strict-tool-use JSON Schema for `browser_actions`.
// Lives here, in the same file as `DefineTools` and the per-action
// dispatch logic, so adding a new action type means editing both the
// runtime extractor and this schema right next to each other.
//
// Design: `actions[].items` is a discriminated union via `anyOf` —
// one small branch per action type. Each branch declares its spec-
// required params PLUS `tab_id` (plain required `integer`, no union)
// for actions that target a tab. Other spec-optional fields are
// dropped from the strict schema entirely.
//
// Why this shape?
//
// Anthropic's strict subset caps the total number of union-typed
// parameters across the schema at 16 ("Schemas contains too many
// parameters with union types..."). An earlier attempt (every spec-
// optional listed with type-array union `["integer","null"]`) hit 32
// across 23 branches — exponential grammar compilation cost, request
// rejected.
//
// Solution: promote `tab_id` to a plain required field on every
// action that takes one. The model echoes the active-tab id back
// from the browser-context attachment, or emits a different tab id
// when it genuinely targets another tab. No nullable types, no
// unions for tab_id — same primitive as `index` or `url`. Other
// spec-optional fields (button, click_count, submit, clear, engine,
// scope, max_chars, max_results, wait_ms, reuse_visible_tab) stay
// out of the strict schema; the runtime extractor's `std::optional`
// defaults take over for those (default engine, default click
// button, etc.).
//
// Strict-subset rules satisfied (Anthropic + OpenAI strict mode):
//   * `additionalProperties: false` on every object
//   * `type` field (plain, no unions) on every property
//   * NO `maxItems` / `minItems` / `pattern` / `format` / `minLength` /
//     `maxLength` / numerical constraints
//   * `anyOf` and `const` are explicitly listed as supported
//   * Total union-typed params: 1 (the items-level anyOf). Well under
//     the 16 cap.
//
// When new action types or per-type params are added in this file,
// update this anyOf list and the runtime extractor in lockstep.

base::DictValue ScalarProp(std::string_view type) {
  base::DictValue d;
  d.Set("type", std::string(type));
  return d;
}

// Chained-builder for one discriminated-union branch. Use either:
//
//   // single-action branch (one `type` value):
//   BranchBuilder("click").Add("index", ScalarProp("integer")).Build();
//
//   // shape-merged branch (multiple action types sharing one shape):
//   BranchBuilder({"wait", "back", "forward"}).Build();
//
// Multi-type construction emits `type: {enum: [...]}` instead of a
// single const, which lets several action types share one branch when
// their property sets are identical. Reduces the total branch count
// the compiler has to enumerate, which keeps Anthropic's grammar
// compiler under its size limit.
//
// All field names get listed in `required[]` per strict-mode rules.
// The branch itself has `additionalProperties: false` so no extra
// keys leak through.
//
// `&&`-qualified members keep the chain operating on rvalues — needed
// because `base::DictValue` is non-copyable, so a non-rvalue chain
// would force copies that don't exist.
class BranchBuilder {
 public:
  explicit BranchBuilder(std::string_view type_const) {
    base::DictValue type_prop;
    type_prop.Set("type", "string");
    type_prop.Set("const", std::string(type_const));
    properties_.Set("type", std::move(type_prop));
    required_.Append("type");
  }

  // Multi-type form: emits `type: {type:"string", enum:[...]}`.
  explicit BranchBuilder(std::initializer_list<std::string_view> types) {
    base::DictValue type_prop;
    type_prop.Set("type", "string");
    base::ListValue type_enum;
    for (auto t : types) {
      type_enum.Append(std::string(t));
    }
    type_prop.Set("enum", std::move(type_enum));
    properties_.Set("type", std::move(type_prop));
    required_.Append("type");
  }

  BranchBuilder(BranchBuilder&&) noexcept = default;
  BranchBuilder& operator=(BranchBuilder&&) noexcept = default;
  BranchBuilder(const BranchBuilder&) = delete;
  BranchBuilder& operator=(const BranchBuilder&) = delete;

  BranchBuilder&& Add(std::string_view name, base::DictValue spec) && {
    properties_.Set(std::string(name), std::move(spec));
    required_.Append(std::string(name));
    return std::move(*this);
  }

  base::DictValue Build() && {
    base::DictValue branch;
    branch.Set("type", "object");
    branch.Set("properties", std::move(properties_));
    branch.Set("required", std::move(required_));
    branch.Set("additionalProperties", false);
    return branch;
  }

 private:
  base::DictValue properties_;
  base::ListValue required_;
};

// Currently unused — the `.StrictInputSchema(...)` call site was
// removed when strict tool use was disabled for browser_actions
// (see the long comment block in `DefineTools` above the
// `NEXUS_END_TOOL` for browser_actions). Kept around as dead code so
// re-enabling strict (with the necessary tab_id-optional refactor) is
// a one-line restore in DefineTools rather than a 150-line rewrite.
// `[[maybe_unused]]` silences `-Werror,-Wunused-function`.
// Canonical example calls forwarded to Anthropic via `input_examples`
// on the wire (see anthropic_provider::TranslateTools and the
// `InputExamples` ToolBuilder method). These are NOT a substitute for
// the system prompt's "actions[] is REQUIRED" guidance — they're an
// in-context anchor the model sees alongside the tool definition.
//
// Three examples were chosen to span the most common batch shapes:
//   1. First-turn search (no prior goal, no actions yet) — anchors
//      `evaluation_previous_goal: "No prior goal..."` plus a single
//      `search` action.
//   2. Mid-flight click (citing an observed element) — anchors
//      reusing an `index` from the latest observation and writing a
//      concrete `next_goal` the next turn can grade.
//   3. Final `done` — anchors task termination with `success: true`
//      and a short `text` summary for the user.
//
// Each example MUST validate against the relaxed `input_schema`
// emitted by NEXUS_INPUT above (Anthropic returns 400 if any entry
// fails validation). Only `type` is required on each action item;
// per-action params (index/url/query) are validated at runtime by
// `RunBrowserActionItem`. Token cost: ~30 tokens × 3 = ~90 tokens
// added to every Anthropic request, paid once per cache miss.
base::ListValue BuildBrowserActionsInputExamples() {
  base::ListValue examples;

  // Example 1 — first-turn search.
  {
    base::DictValue ex;
    ex.Set("evaluation_previous_goal",
           "No prior goal — this is the first turn.");
    ex.Set("memory",
           "User asked for X. Strategy: search the web for X, "
           "then open the most relevant result.");
    ex.Set("next_goal",
           "Search the web for X to surface candidate results.");
    base::ListValue actions;
    base::DictValue act;
    act.Set("type", "search");
    act.Set("query", "your query here");
    actions.Append(std::move(act));
    ex.Set("actions", std::move(actions));
    examples.Append(std::move(ex));
  }

  // Example 2 — click an observed element.
  {
    base::DictValue ex;
    ex.Set("evaluation_previous_goal",
           "Search results page loaded; the first result links to "
           "the target page.");
    ex.Set("memory",
           "User asked for X. Search ran on Google; result list now "
           "shows 10 candidates. First result (index 42) looks most "
           "relevant.");
    ex.Set("next_goal",
           "Click the first organic result to open the target page.");
    base::ListValue actions;
    base::DictValue act;
    act.Set("type", "click");
    act.Set("index", 42);
    actions.Append(std::move(act));
    ex.Set("actions", std::move(actions));
    examples.Append(std::move(ex));
  }

  // Example 3 — final done.
  {
    base::DictValue ex;
    ex.Set("evaluation_previous_goal",
           "Target page read; extracted the answer the user asked for.");
    ex.Set("memory",
           "User asked for X. Found and verified on tab N. "
           "Reporting back and ending the task.");
    ex.Set("next_goal",
           "Finish the task and report the answer to the user.");
    base::ListValue actions;
    base::DictValue act;
    act.Set("type", "done");
    act.Set("success", true);
    act.Set("text",
            "Found X: <one-sentence answer with the concrete result>.");
    actions.Append(std::move(act));
    ex.Set("actions", std::move(actions));
    examples.Append(std::move(ex));
  }

  return examples;
}

[[maybe_unused]] base::DictValue BuildBrowserActionsStrictSchema() {
  base::ListValue branches;

  // Branches are merged by shape: actions sharing the same property
  // set collapse into a single branch with `type: {enum: [...]}`.
  // 23 action types reduce to 11 branches. Anthropic's grammar
  // compiler enumerates fewer paths, keeping the compiled grammar
  // under its size limit.
  //
  // tab_id is plain required `integer` (no nullable union) on every
  // tab-targeting action — the model echoes the active-tab id from
  // the browser-context attachment when no specific tab is intended.
  // Other spec-optional fields (button, click_count, submit, clear,
  // engine, scope, max_chars, max_results, wait_ms,
  // reuse_visible_tab) stay dropped from the strict schema; runtime
  // defaults cover them.

  // Shape: { type, tab_id } — 10 actions
  // (no per-action params beyond tab targeting)
  branches.Append(
      BranchBuilder({"wait", "back", "forward", "close_tab", "activate_tab",
                     "media_play", "media_pause", "media_state", "screenshot",
                     "read_page"})
          .Add("tab_id", ScalarProp("integer"))
          .Build());

  // Shape: { type, tab_id, index } — 4 actions
  branches.Append(
      BranchBuilder({"click", "hover", "scroll_to", "read_element"})
          .Add("index", ScalarProp("integer"))
          .Add("tab_id", ScalarProp("integer"))
          .Build());

  // input — req: index, text, tab_id (unique shape)
  branches.Append(BranchBuilder("input")
                      .Add("index", ScalarProp("integer"))
                      .Add("text", ScalarProp("string"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // select — req: index, value, tab_id (unique shape)
  branches.Append(BranchBuilder("select")
                      .Add("index", ScalarProp("integer"))
                      .Add("value", ScalarProp("string"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // drag_and_drop — req: from_index, to_index, tab_id (unique shape)
  branches.Append(BranchBuilder("drag_and_drop")
                      .Add("from_index", ScalarProp("integer"))
                      .Add("to_index", ScalarProp("integer"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // navigate — req: url, tab_id (unique shape)
  branches.Append(BranchBuilder("navigate")
                      .Add("url", ScalarProp("string"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // scroll — req: direction, distance, tab_id (unique shape)
  branches.Append(BranchBuilder("scroll")
                      .Add("direction", ScalarProp("string"))
                      .Add("distance", ScalarProp("number"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // media_seek — req: seek_time_ms, tab_id (unique shape)
  branches.Append(BranchBuilder("media_seek")
                      .Add("seek_time_ms", ScalarProp("integer"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // find_text — req: query, tab_id (unique shape)
  branches.Append(BranchBuilder("find_text")
                      .Add("query", ScalarProp("string"))
                      .Add("tab_id", ScalarProp("integer"))
                      .Build());

  // Shape: { type, query OR url } — search creates a new tab via a
  // query, create_tab via a URL. Different param names → can't merge.
  // Kept as separate branches.
  // search — req: query (no tab_id)
  branches.Append(
      BranchBuilder("search").Add("query", ScalarProp("string")).Build());

  // create_tab — req: url (no tab_id)
  branches.Append(
      BranchBuilder("create_tab").Add("url", ScalarProp("string")).Build());

  // done — req: success, text (terminates the task; no tab_id)
  branches.Append(BranchBuilder("done")
                      .Add("success", ScalarProp("boolean"))
                      .Add("text", ScalarProp("string"))
                      .Build());

  // The items schema is the discriminated union. Strict mode supports
  // anyOf per Anthropic's structured-outputs JSON Schema subset.
  base::DictValue items;
  items.Set("anyOf", std::move(branches));

  base::DictValue actions_prop;
  actions_prop.Set("type", "array");
  actions_prop.Set("items", std::move(items));

  base::DictValue properties;
  properties.Set("evaluation_previous_goal", ScalarProp("string"));
  properties.Set("memory", ScalarProp("string"));
  properties.Set("next_goal", ScalarProp("string"));
  properties.Set("actions", std::move(actions_prop));

  base::ListValue required;
  required.Append("evaluation_previous_goal");
  required.Append("memory");
  required.Append("next_goal");
  required.Append("actions");

  base::DictValue schema;
  schema.Set("type", "object");
  schema.Set("properties", std::move(properties));
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

}  // namespace

// static
void BrowserTool::DefineTools(ToolRegistry& registry) {
  NEXUS_BEGIN_TOOLS(BrowserTool, registry)

    NEXUS_TOOL(browser_actions, kBrowserActionsDescription)
      // Reflection fields (browser-use mechanism #1 — model self-report).
      // Schema-required so the JSON Schema's `required[]` array nudges
      // every model strongly to think before it acts: each turn must
      // declare what just happened (evaluation_previous_goal), what
      // matters going forward (memory), and what this batch is for
      // (next_goal). The reflections are echoed back onto the result
      // dict in ResolveBrowserActions so they survive into history and
      // feed the next turn — together with the per-action
      // `long_term_memory` action diary (mechanism #2) this gives the
      // model a chronological self-record across turns.
      //
      // EXTRACTION-LENIENT: the C++ method signature uses
      // `std::optional<std::string>` so a model that drops a reflection
      // field still arrives as `std::nullopt` and is silently skipped
      // (no `invalid_args` rejection, no `args:{}` spiral risk). The
      // schema-required + extraction-optional combo gives strong
      // training-time pressure plus runtime forgiveness — see the
      // longer rationale on the `actions` field below.
      NEXUS_INPUT (String, evaluation_previous_goal,
                   "Evaluate whether the previous goal was "
                   "achieved, failed, or still needs work.")
      NEXUS_INPUT (String, memory,
                   "Concise running memory for facts and decisions "
                   "that should persist into the next model step.")
      NEXUS_INPUT (String, next_goal,
                   "The single immediate goal for this action "
                   "batch.")
      // `actions` is REQUIRED on every call. Schema-required (the
      // `required: ["actions"]` array in the JSON schema is the
      // strongest hint the LLM gets — Anthropic / OpenAI models are
      // trained to honor it during tool-use generation), but
      // EXTRACTION-LENIENT: the C++ method signature below uses
      // `std::optional<base::ListValue>`, so an `args:{}` slip from
      // the model still arrives as `std::nullopt` and gets
      // soft-recovered into a no-op `wait` (see
      // `ResolveBrowserActions`) instead of bouncing back as a hard
      // `invalid_args` rejection that would trap the model in an
      // error spiral.
      //
      // The decoupling matters: the schema flag and the extraction
      // type are independent. NEXUS_INPUT here only sets
      // `param.required = true` in BuildHostedToolDefinitionDict
      // (nexus_tool_schema.cc); extraction strictness comes from
      // ToolArgExtractor template specialization on the method
      // signature. So "schema says required, runtime is lenient" is
      // exactly the combo we want — the model is strongly nudged to
      // include actions every time, and the rare slip still gets a
      // soft recovery + <system-reminder> warning rather than
      // exiting with an error.
      //
      // Empirical justification: claude-haiku-4-5 was observed
      // ignoring a prose-only "actions is REQUIRED" hint in the
      // description plus a system-reminder warning on each tool
      // result, sending 5+ consecutive `args:{}` calls. The schema
      // `required[]` array is a stronger training-time signal that
      // the prose alone wasn't carrying.
      NEXUS_INPUT (ArrayOfObject, actions,
                   "Browser Use-style action batch. Send 1-5 "
                   "action objects per call. Each item requires "
                   "a type field such as click, input, navigate, "
                   "search, wait, read_page, find_text, or done, "
                   "plus the params documented in the tool "
                   "description.")
      // Strict-mode JSON Schema variant DELIBERATELY NOT ATTACHED.
      // browser_actions's anyOf-with-12-branches shape is the wrong
      // fit for Anthropic's grammar-constrained sampling: even after
      // shape-merging branches, the compiled grammar pushes against
      // the per-request size limit, and the previous "tab_id required
      // on every branch" mistake locked the model into the few
      // branches without tab_id (search/create_tab/done) when the
      // browser context didn't expose an active tab id. Strict can
      // come back if/when the schema is restructured (the helpers
      // below — BranchBuilder / BuildBrowserActionsStrictSchema /
      // BranchBuilder::AddOptional — stay as dead code so re-enabling
      // is a one-line `.StrictInputSchema(...)` re-add). For now we
      // use a softer mechanism instead — see InputExamples below.
      //
      // Compares to claude_code: 52 tools defined, 8 strict, but
      // every strict tool has a flat schema. claude_code's most
      // complex tool (LSPTool, 9-branch anyOf) is deliberately not
      // strict-marked. Same call we're making here.
      //
      // Anthropic `input_examples`: ships 3 canonical example calls
      // alongside the tool definition (NOT grammar-constrained, just
      // in-context anchors). The model sees what a well-formed
      // browser_actions call looks like — all four required fields
      // populated, a non-empty actions[] batch, realistic content for
      // each — every time the tool definition is sent. This is the
      // softer alternative to strict mode that still addresses the
      // observed Haiku 4.5 empty-args failure mode without the
      // grammar-compilation cost or the structural risk of putting
      // an anyOf-12 schema under grammar constraints. Other providers
      // (OpenAI / Google / Ollama) ignore the field; only Anthropic
      // surfaces it on the wire.
      _nexus_tool_builder.InputExamples(
          BuildBrowserActionsInputExamples());
    NEXUS_END_TOOL(Object)

  NEXUS_END_TOOLS()
}

BrowserTool::BrowserTool(Profile* profile, const std::string& session_key)
    : ToolBase(profile, session_key) {}

BrowserTool::~BrowserTool() {
  // Defensive — if OnStop wasn't called explicitly (e.g. via
  // ToolsAdapter::OnSessionStopped), run it now so the actor task
  // never leaks. Idempotent: task_id_ is reset by OnStop.
  OnStop("destructor");
}

void BrowserTool::OnStop(const std::string& reason) {
  LOG(INFO) << "[BrowserTool] OnStop session=" << GetSessionKey()
            << " reason=" << reason
            << " has_task=" << task_id_.has_value()
            << " registry_size=" << node_registry_.size();
  if (task_id_.has_value()) {
    if (auto* service = actor::ActorKeyedService::Get(GetProfile())) {
      service->StopTask(*task_id_, MapStopReason(reason));
    }
    task_id_.reset();
  }
  task_state_subscription_ = {};
  actor_task_paused_ = false;
  for (const scoped_refptr<PausableTimeout>& timeout : active_timeouts_) {
    timeout->Stop();
  }
  active_timeouts_.clear();
  // Stale node ids from a stopped session must never feed back
  // into a fresh session's actor calls.
  node_registry_.Clear();
}

scoped_refptr<PausableTimeout> BrowserTool::RegisterPausableTimeout(
    base::TimeDelta budget,
    std::string name,
    base::OnceClosure on_timeout) {
  PruneSettledTimeouts();
  auto timeout = base::MakeRefCounted<PausableTimeout>(
      budget, std::move(name), std::move(on_timeout));
  active_timeouts_.push_back(timeout);
  timeout->Start(actor_task_paused_);
  return timeout;
}

ToolCallback BrowserTool::PausableBudgetWrap(ToolCallback inner,
                                             base::TimeDelta budget,
                                             std::string action_name) {
  if (!budget.is_positive()) {
    return inner;
  }
  auto holder = base::MakeRefCounted<PausableToolCallbackHolder>(
      std::move(inner), action_name);
  scoped_refptr<PausableTimeout> timeout = RegisterPausableTimeout(
      budget, action_name,
      base::BindOnce(
          [](base::WeakPtr<BrowserTool> tool,
             scoped_refptr<PausableToolCallbackHolder> h, std::string name) {
            if (tool) {
              tool->StopCurrentActorTaskForTimeout(name);
            }
            h->OnTimeout();
          },
          weak_ptr_factory_.GetWeakPtr(), holder, action_name));
  holder->SetTimeout(timeout);
  return ToolCallback(
      base::BindOnce(
          [](scoped_refptr<PausableToolCallbackHolder> h, base::Value v) {
            h->Resolve(std::move(v));
          },
          holder),
      base::BindOnce(
          [](scoped_refptr<PausableToolCallbackHolder> h, std::string c,
             std::string m) { h->Reject(std::move(c), std::move(m)); },
          holder));
}

namespace {

bool ActorStatePausesTimeouts(actor::ActorTask::State state) {
  return state == actor::ActorTask::State::kPausedByActor ||
         state == actor::ActorTask::State::kPausedByUser ||
         state == actor::ActorTask::State::kWaitingOnUser;
}

bool ActorStateResumesTimeouts(actor::ActorTask::State state) {
  return state == actor::ActorTask::State::kCreated ||
         state == actor::ActorTask::State::kActing ||
         state == actor::ActorTask::State::kReflecting;
}

}  // namespace

void BrowserTool::OnActorTaskStateChanged(actor::TaskId task_id,
                                          actor::ActorTask::State state) {
  if (!task_id_.has_value() || task_id != *task_id_) {
    return;
  }
  if (ActorStatePausesTimeouts(state)) {
    actor_task_paused_ = true;
    PauseActiveTimeouts();
    return;
  }
  if (actor_task_paused_ && ActorStateResumesTimeouts(state)) {
    actor_task_paused_ = false;
    ResumeActiveTimeouts();
  }
}

void BrowserTool::PauseActiveTimeouts() {
  for (const scoped_refptr<PausableTimeout>& timeout : active_timeouts_) {
    timeout->Pause();
  }
}

void BrowserTool::ResumeActiveTimeouts() {
  for (const scoped_refptr<PausableTimeout>& timeout : active_timeouts_) {
    timeout->Resume();
  }
  PruneSettledTimeouts();
}

void BrowserTool::PruneSettledTimeouts() {
  std::erase_if(active_timeouts_, [](const scoped_refptr<PausableTimeout>& t) {
    return !t || t->settled();
  });
}

void BrowserTool::StopCurrentActorTaskForTimeout(std::string_view reason) {
  if (!task_id_.has_value()) {
    return;
  }
  if (auto* service = actor::ActorKeyedService::Get(GetProfile())) {
    LOG(WARNING) << "[BrowserTool] stopping task after timeout session="
                 << GetSessionKey() << " reason=" << reason;
    service->StopTask(*task_id_, actor::ActorTask::StoppedReason::kChromeFailure);
  }
  for (const scoped_refptr<PausableTimeout>& timeout : active_timeouts_) {
    timeout->Stop();
  }
  active_timeouts_.clear();
  task_id_.reset();
  task_state_subscription_ = {};
  actor_task_paused_ = false;
}

// =============================================================================
// Phase 2 actor wiring — task ownership + dispatch helpers.
// =============================================================================

std::optional<actor::TaskId> BrowserTool::EnsureTaskCreated(
    ToolCallback& done) {
  if (task_id_.has_value()) {
    return task_id_;
  }
  Profile* profile = GetProfile();
  if (!profile) {
    std::move(done).Reject("no_profile",
                           "actor task creation: no profile available");
    return std::nullopt;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject(
        "no_actor_service",
        "actor task creation: ActorKeyedService unavailable");
    return std::nullopt;
  }
  const auto* policy_checker = GetPolicyChecker(profile);
  if (!policy_checker) {
    std::move(done).Reject(
        "no_policy_checker",
        "actor task creation: enterprise policy checker unavailable");
    return std::nullopt;
  }

  actor::webui::mojom::TaskOptionsPtr options =
      actor::webui::mojom::TaskOptions::New();
  options->title = base::StrCat({"nexus-browser-tools:", GetSessionKey()});

  task_id_ = service->CreateTaskWithOptions(
      policy_checker, std::move(options),
      /*delegate=*/base::WeakPtr<actor::ActorTaskDelegate>());
  task_state_subscription_ = service->AddTaskStateChangedCallback(
      base::BindRepeating(&BrowserTool::OnActorTaskStateChanged,
                          weak_ptr_factory_.GetWeakPtr()));
  return task_id_;
}

void BrowserTool::RunActorAction(apc::Actions actions,
                                 std::string_view action_name,
                                 ToolCallback done) {
  // Wrap `done` with a per-action wall-clock budget so a hung
  // CDP/WebSocket can't deadlock the loop. The settle-once timer
  // races the legitimate PerformActions → BuildActionsResultWithObservations
  // → OnActionsResultBuilt completion chain; whichever finishes first
  // resolves the underlying ToolCallback. JS-side step_timeout still
  // protects the whole-step level; this is the finer-grained native
  // safety net for individual actor calls.
  done = PausableBudgetWrap(std::move(done), action_timeout_,
                            std::string(action_name));

  std::optional<actor::TaskId> task_id = EnsureTaskCreated(done);
  if (!task_id.has_value()) {
    return;  // EnsureTaskCreated already rejected `done`.
  }
  auto* service = actor::ActorKeyedService::Get(GetProfile());
  if (!service) {
    std::move(done).Reject(
        "no_actor_service",
        base::StrCat({action_name, ": ActorKeyedService disappeared"}));
    return;
  }

  actions.set_task_id(task_id->value());

  actor::BuildToolRequestResult tool_request_result =
      actor::BuildToolRequest(actions);
  if (!tool_request_result.has_value()) {
    std::move(done).Reject(
        "invalid_action",
        base::StrCat({action_name, ": failed to build ToolRequest"}));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  actor::ActorTaskMetadata task_metadata;

  service->PerformActions(
      *task_id, std::move(tool_request_result.value()),
      std::move(task_metadata),
      base::BindOnce(&BrowserTool::OnActorPerformActionsDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::string(action_name), start_time));
}

void BrowserTool::OnActorPerformActionsDone(
    ToolCallback done,
    std::string action_name,
    base::TimeTicks start_time,
    actor::mojom::ActionResultCode result_code,
    std::optional<size_t> index_of_failed_action,
    std::vector<actor::ActionResultWithLatencyInfo> action_results) {
  // PerformActions has settled. Hand off to the actor framework's
  // observation builder so the LLM gets a fresh page snapshot back.
  // Without this stage the LLM has no way to know what the action did.
  Profile* profile = GetProfile();
  if (!profile) {
    std::move(done).Reject(
        "no_profile",
        base::StrCat({action_name, ": Profile gone before observation"}));
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject(
        "no_actor_service",
        base::StrCat({action_name,
                      ": ActorKeyedService disappeared before observation"}));
    return;
  }
  if (!task_id_.has_value()) {
    std::move(done).Reject(
        "task_missing",
        base::StrCat({action_name, ": session task no longer present"}));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_);
  if (!task) {
    std::move(done).Reject(
        "task_missing",
        base::StrCat({action_name, ": actor task no longer exists"}));
    return;
  }
  // Defensive: don't kick off the observation pipeline when the task has
  // no tabs. Upstream `actor::BuildActionsResultWithObservations` →
  // `ActorKeyedService::RequestTabObservation` walks the task's tabs and
  // calls `ActorTabData::DidObserveContent(content)` on each, which
  // unconditionally `emplace()`s onto an `optional<AnnotatedPageContent>`.
  // The `emplace` `reset()`s the prior value first — and we have observed
  // upstream crashes when the prior value's protobuf arena memory was
  // freed before this turn (chrome/browser/actor/actor_tab_data.cc:28,
  // `optional<AnnotatedPageContent>::reset()` faulting in the proto's
  // destructor). Skipping the pipeline when there are no tabs means we
  // don't even enter that code path. Other safety: callers like `done`
  // reach this path with `task->GetTabs()` populated only when the
  // session has at least one live tab, so this is a tight guard for the
  // genuinely-empty edge case (e.g. last tab closed mid-batch).
  if (task->GetTabs().empty()) {
    std::move(done).Reject(
        "no_tabs",
        base::StrCat({action_name,
                      ": actor task has no live tabs for observation"}));
    return;
  }

  actor::BuildActionsResultWithObservations(
      *profile, start_time, result_code, index_of_failed_action,
      std::move(action_results), *task,
      /*skip_async_observation_information=*/false,
      base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::move(action_name)));
}

void BrowserTool::OnActionsResultBuilt(
    ToolCallback done,
    std::string action_name,
    base::TimeTicks /*start_time*/,
    actor::mojom::ActionResultCode result_code,
    std::optional<size_t> /*index_of_failed_action*/,
    std::vector<actor::ActionResultWithLatencyInfo> /*action_results*/,
    actor::TaskId /*task_id*/,
    bool /*skip_async_observation_information*/,
    std::unique_ptr<apc::ActionsResult> result_proto,
    std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> /*journal*/) {
  // Node registry ACCUMULATES across observations now — indexes
  // (== APC content_node_id) are stable for a document's lifetime,
  // so the LLM can refer to elements from prior turns. Stale indexes
  // (e.g. element removed from DOM since last seen) surface as a
  // clean actor "stale" error rather than a misdirected action.
  // The registry is only cleared on session OnStop or, optionally,
  // on per-tab close via NodeRegistry::ClearTab.

  base::DictValue observation;
  // Defensive: `result_proto` should always be non-null when the
  // observation builder ran successfully, but the upstream actor pipeline
  // can deliver a null on rare error paths (profile teardown race,
  // FetchPageContext failure with a fallback empty result). Treat a
  // missing or empty proto as "no observation" instead of trying to walk
  // nonexistent tabs — `BuildObservationDict` is safe on empty tabs() but
  // explicit emptiness here is clearer for diagnostics and avoids
  // exercising any latent upstream paths on degenerate inputs.
  if (result_proto && result_proto->tabs_size() > 0) {
    observation = BuildObservationDict(*result_proto);
  }

  base::DictValue result;
  result.Set("action", action_name);
  if (!observation.empty()) {
    result.Set("observation", std::move(observation));
  }

  if (!actor::IsOk(result_code)) {
    // Tool result still includes the observation so the LLM can see
    // why the page rejected the action (e.g. element became stale).
    result.Set("ok", false);
    result.Set("result", DescribeActorError(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }

  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

namespace {

// Maps the proto's CONTENT_ATTRIBUTE_* enum onto short type names the
// LLM has historically seen. Mirrors the labels the JS-side
// observationParser used.
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

// Pulls the best visible-text snippet out of a node's attributes.
// Order of preference matches the JS-side observationParser: explicit
// `label` first (e.g. aria-label), then a leaf TEXT node's content.
std::string ExtractNodeText(const apc::ContentAttributes& attrs) {
  if (attrs.has_label() && !attrs.label().empty()) {
    return attrs.label();
  }
  if (attrs.has_text_data() && !attrs.text_data().text_content().empty()) {
    return attrs.text_data().text_content();
  }
  return std::string();
}

// Walks a node's descendants and concatenates the first useful text-like
// snippet found (TEXT content_text, IMAGE caption, SVG_ROOT inner_text,
// or descendant `label`). Used as a fallback when an actionable wrapper
// (an <a> around an icon, a <button> around a <span>, etc.) has no
// text of its own — without this fallback the LLM sees a bare
// `{index, type}` and can't tell what the element is for.
//
// Stops at the first non-empty snippet, capped at 80 chars after a
// UTF-8-safe trim, so a deeply nested div tree doesn't poison the
// observation with paragraph-sized strings.
constexpr int kDescendantTextSnippetMax = 80;

// Local UTF-8-safe truncate. Same algorithm as EnsureUtf8TailComplete
// further down in the file (separate anon namespace, so the forward
// decl wouldn't link). Walks back from a fixed byte budget to the most
// recent UTF-8 leading byte so a Chinese / Japanese / Korean snippet
// doesn't crash base::Value's UTF-8 validator on the next Set call.
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

std::string ExtractDescendantTextSnippet(const apc::ContentNode& node) {
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (child.has_content_attributes()) {
      const apc::ContentAttributes& cattrs = child.content_attributes();
      // Skip nested actionable nodes — their text belongs to their own
      // observation entry, not the wrapper's.
      if (cattrs.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR ||
          cattrs.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL) {
        continue;
      }
      if (cattrs.has_label() && !cattrs.label().empty()) {
        return TruncateUtf8Snippet(cattrs.label(),
                                    kDescendantTextSnippetMax);
      }
      if (cattrs.has_text_data() &&
          !cattrs.text_data().text_content().empty()) {
        return TruncateUtf8Snippet(cattrs.text_data().text_content(),
                                    kDescendantTextSnippetMax);
      }
      if (cattrs.has_image_data() &&
          !cattrs.image_data().image_caption().empty()) {
        return TruncateUtf8Snippet(cattrs.image_data().image_caption(),
                                    kDescendantTextSnippetMax);
      }
      if (cattrs.has_svg_root_data() &&
          !cattrs.svg_root_data().inner_text().empty()) {
        return TruncateUtf8Snippet(cattrs.svg_root_data().inner_text(),
                                    kDescendantTextSnippetMax);
      }
    }
    // Recurse — actionable wrappers often have a deeply nested
    // <span><span>text</span></span> structure (icon button libraries).
    std::string nested = ExtractDescendantTextSnippet(child);
    if (!nested.empty()) {
      return nested;
    }
  }
  return std::string();
}

// Walks a node's descendants and returns the first non-empty visible
// bounding box. Used as a fallback when the wrapper's own geometry is
// missing or zero-sized, which is common for inline anchors that
// inherit their hit area from a child <img> or <span>.
std::optional<base::Value> ExtractDescendantBoundingBox(
    const apc::ContentNode& node) {
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (child.has_content_attributes() &&
        child.content_attributes().has_geometry()) {
      const apc::Geometry& geo = child.content_attributes().geometry();
      if (geo.has_visible_bounding_box()) {
        const apc::BoundingRect& r = geo.visible_bounding_box();
        if (r.width() > 0 || r.height() > 0 || r.x() != 0 || r.y() != 0) {
          base::ListValue box;
          box.Append(r.x());
          box.Append(r.y());
          box.Append(r.width());
          box.Append(r.height());
          return base::Value(std::move(box));
        }
      }
    }
    if (auto nested = ExtractDescendantBoundingBox(child)) {
      return nested;
    }
  }
  return std::nullopt;
}

// True when this node has enough interaction signal to be a target
// the LLM might pick. Mirrors the JS-side filter: clickable /
// focusable / form_control / anchor.
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

// Maps the proto's FORM_CONTROL_TYPE_* enum to the same short names HTML
// developers use ("text", "search", "checkbox", …). The LLM relies on
// these to tell a search box apart from a number input or radio button
// — without it, every form control reads as `form_control`.
const char* FormControlTypeName(apc::FormControlType t) {
  switch (t) {
    case apc::FORM_CONTROL_TYPE_CONTENT_EDITABLE:
      return "contenteditable";
    case apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX:
      return "checkbox";
    case apc::FORM_CONTROL_TYPE_INPUT_EMAIL:
      return "email";
    case apc::FORM_CONTROL_TYPE_INPUT_MONTH:
      return "month";
    case apc::FORM_CONTROL_TYPE_INPUT_NUMBER:
      return "number";
    case apc::FORM_CONTROL_TYPE_INPUT_PASSWORD:
      return "password";
    case apc::FORM_CONTROL_TYPE_INPUT_RADIO:
      return "radio";
    case apc::FORM_CONTROL_TYPE_INPUT_SEARCH:
      return "search";
    case apc::FORM_CONTROL_TYPE_INPUT_TELEPHONE:
      return "tel";
    case apc::FORM_CONTROL_TYPE_INPUT_TEXT:
      return "text";
    case apc::FORM_CONTROL_TYPE_INPUT_URL:
      return "url";
    case apc::FORM_CONTROL_TYPE_SELECT_ONE:
      return "select";
    case apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE:
      return "select-multiple";
    default:
      return nullptr;
  }
}

// Heuristic ARIA-role for an element. Pulls from the proto's
// `annotated_roles` first (the renderer's role layer already merges
// computed-aria + html-tag), falls back to the form_control type for
// inputs and to "link" for anchors / "button" for clickable form
// controls. Keeping the fallback narrow avoids guessing a role for
// every clickable div, which would dilute the signal.
std::string ExtractRole(const apc::ContentAttributes& attrs) {
  // (We deliberately don't enumerate every AnnotatedRoleType here —
  // the proto's enum is large and frequently extended; we forward the
  // first role with a recognizable string form by reading the enum's
  // descriptor at runtime would be heavier than warranted. The label /
  // input_type fields below carry most of the disambiguation power.)
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR) {
    return "link";
  }
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL &&
      attrs.has_form_control_data()) {
    apc::FormControlType t = attrs.form_control_data().form_control_type();
    if (t == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX) return "checkbox";
    if (t == apc::FORM_CONTROL_TYPE_INPUT_RADIO) return "radio";
    if (t == apc::FORM_CONTROL_TYPE_SELECT_ONE ||
        t == apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE) {
      return "combobox";
    }
    if (t == apc::FORM_CONTROL_TYPE_INPUT_SEARCH) return "searchbox";
    return "textbox";  // text / email / password / url / number / tel /
                       // contenteditable all behave as text input for
                       // the LLM's purposes
  }
  if (attrs.has_interaction_info() &&
      attrs.interaction_info().is_clickable()) {
    return "button";
  }
  return std::string();
}

// Compresses a Geometry message into a 4-tuple [x, y, w, h] that the
// LLM can reason about cheaply (e.g. "clickable element near top of
// viewport"). Returns std::nullopt when the visible bounding box is
// empty, since an empty box means "not hit-testable / offscreen" and
// shipping zeros would be misleading.
std::optional<base::Value> ExtractBoundingBox(
    const apc::ContentAttributes& attrs) {
  if (!attrs.has_geometry()) {
    return std::nullopt;
  }
  const apc::Geometry& geo = attrs.geometry();
  if (!geo.has_visible_bounding_box()) {
    return std::nullopt;
  }
  const apc::BoundingRect& r = geo.visible_bounding_box();
  // Skip empty rectangles (proto's BoundingRect zero-init = offscreen).
  if (r.width() == 0 && r.height() == 0 && r.x() == 0 && r.y() == 0) {
    return std::nullopt;
  }
  base::ListValue box;
  box.Append(r.x());
  box.Append(r.y());
  box.Append(r.width());
  box.Append(r.height());
  return base::Value(std::move(box));
}

}  // namespace

void BrowserTool::WalkContentNode(const apc::ContentNode& node,
                                   const std::string& document_id,
                                   int tab_id,
                                   base::ListValue& out_elements) {
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    if (IsActionable(attrs) && attrs.has_common_ancestor_dom_node_id()) {
      // Use the APC's content_node_id directly as the LLM-facing
      // index. Stable for the document's lifetime in the renderer,
      // so the LLM can refer to the same DOM element by the same index
      // across observations / turns.
      const int node_id = attrs.common_ancestor_dom_node_id();
      const char* type_name = ContentAttributeTypeName(attrs.attribute_type());
      std::string text = ExtractNodeText(attrs);
      // Wrapper anchors / buttons usually carry their visible signal in
      // descendants (icon caption, inner span text). Fall back so the
      // LLM sees `text: "Sign in"` instead of a bare `{index, type}`.
      if (text.empty()) {
        text = ExtractDescendantTextSnippet(node);
      }

      NodeRegistry::Entry entry;
      entry.document_id = document_id;
      entry.tab_id = tab_id;
      entry.type = type_name;
      entry.text = text;
      node_registry_.Set(node_id, entry);

      // Build the rich element payload. Required: index + type.
      // Everything else is opportunistic — only emit a key when the
      // proto carries a non-empty value, so the serialized observation
      // stays small on element-heavy pages (Baidu's homepage is ~80
      // form controls) while still giving the LLM enough signal to
      // tell a search box ("role=searchbox, placeholder=百度一下")
      // apart from a generic submit button.
      //
      // `type` and `role` are intentionally separate fields: `type` is
      // the proto-level structural kind (link / form_control / element
      // / image), `role` is the ARIA-derived interaction role
      // (searchbox / textbox / checkbox / combobox / button / link).
      // For anchors they coincide (both "link"); for form controls
      // they diverge ("form_control" vs "textbox") and the role plus
      // the optional input_type / placeholder give the LLM the shape
      // it needs to disambiguate inputs.
      base::DictValue elem;
      elem.Set("index", node_id);
      elem.Set("type", type_name);
      if (!text.empty()) {
        elem.Set("text", text);
      }
      // ARIA role / heuristic role (link / button / textbox / …).
      std::string role = ExtractRole(attrs);
      if (!role.empty()) {
        elem.Set("role", std::move(role));
      }
      // Form control specifics — placeholder, current value, input
      // type ("text" / "search" / "email" / "checkbox" / …),
      // is_required / is_checked. These solve the "search input vs.
      // search button" disambiguation the previous schema couldn't.
      if (attrs.has_form_control_data()) {
        const apc::FormControlData& fc = attrs.form_control_data();
        if (fc.has_placeholder() && !fc.placeholder().empty()) {
          elem.Set("placeholder", fc.placeholder());
        }
        if (fc.has_field_value() && !fc.field_value().empty()) {
          elem.Set("value", fc.field_value());
        }
        if (fc.has_field_name() && !fc.field_name().empty()) {
          elem.Set("name", fc.field_name());
        }
        if (fc.has_form_control_type()) {
          if (const char* it = FormControlTypeName(fc.form_control_type())) {
            elem.Set("input_type", it);
          }
        }
        if (fc.has_is_required() && fc.is_required()) {
          elem.Set("is_required", true);
        }
        if (fc.has_is_checked()) {
          // Always emit for checkable controls so the LLM can read
          // current state (not just `is_checked: true` when checked).
          if (fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
              fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_RADIO) {
            elem.Set("is_checked", fc.is_checked());
          }
        }
      }
      // Anchor specifics — destination URL. Critical for telling search
      // result links apart on a results page.
      if (attrs.has_anchor_data() && !attrs.anchor_data().url().empty()) {
        elem.Set("href", attrs.anchor_data().url());
      }
      // Viewport bounding box — `[x, y, w, h]`. Useful when the LLM has
      // multiple candidate matches and wants to prefer the one near
      // the top of the viewport (e.g. first search result). Inline
      // anchors often have an empty box at the wrapper itself but a
      // real one on a descendant <img>/<span>; fall back to that so
      // every interactable element ships some position info.
      auto box = ExtractBoundingBox(attrs);
      if (!box) {
        box = ExtractDescendantBoundingBox(node);
      }
      if (box) {
        elem.Set("box", std::move(*box));
      }

      // Drop bare wrapper entries that we couldn't enrich with any
      // text / href / box / form-state. Keeping them would just inflate
      // the observation with unselectable noise — the LLM has nothing
      // to cite as element_reason and can't tell what the index targets.
      // node_registry_.Set above still ran, so a later observation that
      // DOES enrich this index can win; we just skip the wire append
      // and the normalized-projection update for this turn.
      const bool has_useful_signal =
          !text.empty() || elem.contains("href") || elem.contains("box") ||
          elem.contains("placeholder") || elem.contains("value") ||
          elem.contains("name") || elem.contains("input_type") ||
          elem.contains("is_checked");
      if (!has_useful_signal) {
        // Recurse into children below so descendants still surface.
        for (const apc::ContentNode& child : node.children_nodes()) {
          WalkContentNode(child, document_id, tab_id, out_elements);
        }
        return;
      }

      out_elements.Append(std::move(elem));

      // Append an identifier-free projection of this element to the
      // rolling normalized projection used for dom_hash. Identifiers
      // (content_node_id, tab_id, document_id) are excluded so the projection
      // captures the page's interactive surface SHAPE, not bookkeeping.
      // Form-state fields (value, is_checked) are INCLUDED — typing
      // into a field IS meaningful page-state change. Walk order is
      // deterministic for a given DOM, so the projection is stable
      // across re-observations of the same page.
      base::StrAppend(&last_observation_normalized_projection_,
                      {"|", type_name, ";"});
      if (!text.empty()) {
        base::StrAppend(&last_observation_normalized_projection_,
                        {"t=", text, ";"});
      }
      if (attrs.has_form_control_data()) {
        const apc::FormControlData& fc = attrs.form_control_data();
        if (fc.has_form_control_type()) {
          if (const char* it = FormControlTypeName(fc.form_control_type())) {
            base::StrAppend(&last_observation_normalized_projection_,
                            {"it=", it, ";"});
          }
        }
        if (fc.has_placeholder() && !fc.placeholder().empty()) {
          base::StrAppend(&last_observation_normalized_projection_,
                          {"ph=", fc.placeholder(), ";"});
        }
        if (fc.has_field_value() && !fc.field_value().empty()) {
          base::StrAppend(&last_observation_normalized_projection_,
                          {"v=", fc.field_value(), ";"});
        }
        if (fc.has_is_required() && fc.is_required()) {
          last_observation_normalized_projection_.append("r=1;");
        }
        if (fc.has_is_checked() &&
            (fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
             fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_RADIO)) {
          base::StrAppend(&last_observation_normalized_projection_,
                          {"c=", fc.is_checked() ? "1" : "0", ";"});
        }
      }
      if (attrs.has_anchor_data() && !attrs.anchor_data().url().empty()) {
        base::StrAppend(&last_observation_normalized_projection_,
                        {"h=", attrs.anchor_data().url(), ";"});
      }
      ++last_observation_interactive_count_;
      // focus_target_id is set once per observation from
      // `apc_data.page_interaction_info().focused_node_id()` in
      // BuildObservationDict — it's the stable DOM content_node_id of
      // the focused element, not anything we'd derive from
      // WalkContentNode's element walk.
    }
  }
  for (const apc::ContentNode& child : node.children_nodes()) {
    WalkContentNode(child, document_id, tab_id, out_elements);
  }
}

base::DictValue BrowserTool::BuildObservationDict(
    const apc::ActionsResult& result_proto) {
  // Reset the agent-meta snapshot at the start of every observation.
  // The "primary" tab's URL/focus/tab_id are surfaced to the JS-side
  // detector and to per-action stop-detection (`pre_action_tab_id`,
  // `tab_changed`).
  //
  // IMPORTANT: We deliberately do NOT use "first tab in
  // result_proto.tabs()" as the primary tab. The upstream
  // `BuildActionsResultWithObservations` populates result_proto.tabs()
  // by iterating `task.GetLastActedTabs()`, which is an
  // `absl::flat_hash_set<tabs::TabHandle>` — and there's an upstream
  // TODO (`actor_task.cc:742`, crbug.com/420669167) admitting that
  // method actually returns ALL controlled tabs, not just the
  // most-recently-acted ones. Once the agent has touched a tab in this
  // task, it stays in `controlled_tabs_` for the task's lifetime, so
  // multi-batch sessions accumulate ≥2 entries.
  //
  // Hash set iteration order is non-deterministic across calls, so
  // "first tab" jumps randomly between batches even when the user is
  // sitting on a single tab. That breaks:
  //   - JS-side per-tab keep-newest strip dedup (same logical tab
  //     reads as N distinct tab_ids → cap of 2 keeps too many full
  //     observations).
  //   - tab-change stop-detection (`pre_action_tab_id != post_action_tab_id`
  //     fires spuriously and chops batches short).
  //
  // Fix: prefer the active tab handle (from
  // `chrome::FindLastActiveWithProfile` → `TabStripModel::GetActiveTab`)
  // as the deterministic primary tab when it's present in the
  // observation. Fall back to first-seen for the genuinely-non-active
  // case (agent operating on a background tab via explicit `tab_id`).
  last_observation_url_.clear();
  last_observation_title_.clear();
  last_observation_interactive_count_ = 0;
  last_observation_tab_id_.reset();
  last_observation_normalized_projection_.clear();
  last_observation_focus_target_present_ = false;
  last_observation_focus_target_id_.clear();

  // Snapshot the active tab handle once. `GetActiveTabHandleId` returns
  // nullopt if the profile has no live Browser/active tab; in that case
  // we fall through to first-seen semantics.
  const std::optional<int32_t> active_tab_handle_id =
      GetActiveTabHandleId(GetProfile());

  // Per-tab values captured during the walk so we can pick the primary
  // signals AFTER deciding which tab is primary (active vs first-seen).
  // Keeps the loop body close to the original layout — we just defer
  // assignment to the member fields.
  struct TabPrimarySnapshot {
    int tab_id = 0;
    std::string url;
    std::string title;
    bool focus_present = false;
    std::string focus_target_id;
  };
  std::optional<TabPrimarySnapshot> first_seen_snapshot;
  std::optional<TabPrimarySnapshot> active_snapshot;

  base::DictValue observation;
  base::ListValue tabs;
  for (const apc::TabObservation& tab_obs : result_proto.tabs()) {
    base::DictValue tab_dict;
    tab_dict.Set("tab_id", tab_obs.id());
    if (tab_obs.has_screenshot() && !tab_obs.screenshot().empty()) {
      tab_dict.Set("has_screenshot", true);
    }

    TabPrimarySnapshot this_tab;
    this_tab.tab_id = tab_obs.id();

    base::ListValue elements;
    if (tab_obs.has_annotated_page_content()) {
      const apc::AnnotatedPageContent& apc_data =
          tab_obs.annotated_page_content();

      // Pull the document_identifier from the main frame's metadata —
      // it pairs with content_node_id when building actor targets.
      std::string document_id;
      if (apc_data.has_main_frame_data() &&
          apc_data.main_frame_data().has_document_identifier()) {
        document_id =
            apc_data.main_frame_data().document_identifier().serialized_token();
      }
      if (apc_data.has_main_frame_data()) {
        if (!apc_data.main_frame_data().url().empty()) {
          tab_dict.Set("url", apc_data.main_frame_data().url());
          this_tab.url = apc_data.main_frame_data().url();
        }
        if (!apc_data.main_frame_data().title().empty()) {
          tab_dict.Set("title", apc_data.main_frame_data().title());
          this_tab.title = apc_data.main_frame_data().title();
        }
      }

      // Stable focus signal for multi-act guards. The proto's
      // `page_interaction_info.focused_node_id` is the focused
      // element's DOM content_node_id — same identifier space as
      // the LLM-facing index, so multi-act guards, focus tracking,
      // and registry lookups all share the same key.
      if (apc_data.has_page_interaction_info() &&
          apc_data.page_interaction_info().has_focused_node_id() &&
          apc_data.page_interaction_info().focused_node_id() != 0) {
        this_tab.focus_present = true;
        this_tab.focus_target_id = base::NumberToString(
            apc_data.page_interaction_info().focused_node_id());
      }

      const int tab_id_for_registry =
          apc_data.has_tab_id() ? apc_data.tab_id() : tab_obs.id();

      if (apc_data.has_root_node()) {
        WalkContentNode(apc_data.root_node(), document_id, tab_id_for_registry,
                        elements);
      }
    }
    tab_dict.Set("elements", std::move(elements));
    tabs.Append(std::move(tab_dict));

    if (!first_seen_snapshot.has_value()) {
      first_seen_snapshot = this_tab;
    }
    if (active_tab_handle_id.has_value() &&
        tab_obs.id() == *active_tab_handle_id) {
      active_snapshot = this_tab;
    }
  }

  // Pick the primary tab: active if present in the observation,
  // otherwise first-seen. This is the only place `last_observation_*`
  // primary fields get assigned, so they're guaranteed deterministic
  // across batches whenever the user sits on a stable foreground tab.
  const std::optional<TabPrimarySnapshot>& primary =
      active_snapshot.has_value() ? active_snapshot : first_seen_snapshot;
  if (primary.has_value()) {
    last_observation_tab_id_ = primary->tab_id;
    last_observation_url_ = primary->url;
    last_observation_title_ = primary->title;
    last_observation_focus_target_present_ = primary->focus_present;
    last_observation_focus_target_id_ = primary->focus_target_id;
  }

  observation.Set("tabs", std::move(tabs));
  return observation;
}

void BrowserTool::RejectNotYetImplemented(std::string_view tool_name,
                                          ToolCallback done) {
  std::move(done).Reject(
      "not_yet_implemented",
      base::StrCat({"C++ implementation of ", tool_name,
                    " has not landed yet. The JS path is the live "
                    "executor; reaching this code means the JS-side "
                    "catalog was bypassed — check tools.ts ordering."}));
}

const NodeRegistry::Entry* BrowserTool::LookupNode(
    int node_id,
    std::optional<int> tab_id,
    ToolCallback& done,
    std::string_view tool_name) {
  const NodeRegistry::Entry* entry = tab_id.has_value()
      ? node_registry_.FindIn(node_id, *tab_id)
      : node_registry_.Find(node_id);
  if (!entry) {
    std::move(done).Reject(
        "index_not_found",
        base::StrCat({tool_name, ": index ",
                      base::NumberToString(node_id),
                      tab_id.has_value()
                          ? base::StrCat({" in tab ",
                                          base::NumberToString(*tab_id)})
                          : std::string(),
                      " not in this session's registry. Run a "
                      "screenshot or read_page to "
                      "observe the page first."}));
  }
  return entry;
}

// =============================================================================
// Tool bodies
// =============================================================================
//
// Action tools build a single-action `apc::Actions` proto and dispatch
// through `RunActorAction`. Tools that take an optional `tab_id` resolve
// to the active tab via `ResolveTabIdOrReject` when the LLM omits it.
//
// Observation actions (`screenshot` / `read_page` / `find_text`) skip
// the PerformActions path: they call
// `RunTabObservation`, which marks the target tab via
// `ActorTask::ObserveTabOnce` and then runs
// `BuildActionsResultWithObservations` directly. `read_element`
// is fully synchronous — it just reads the per-session element registry.

namespace {

constexpr size_t kMaxBrowserActionsPerStep = 5;

std::optional<int> OptionalIntArg(const base::DictValue& action,
                                  std::string_view name) {
  return action.FindInt(name);
}

std::optional<double> OptionalNumberArg(const base::DictValue& action,
                                        std::string_view name) {
  std::optional<double> as_double = action.FindDouble(name);
  if (as_double.has_value()) {
    return as_double;
  }
  std::optional<int> as_int = action.FindInt(name);
  if (as_int.has_value()) {
    return static_cast<double>(*as_int);
  }
  return std::nullopt;
}

std::optional<int> OptionalElementIndex(const base::DictValue& action) {
  return action.FindInt("index");
}

std::optional<int> RequiredElementIndex(const base::DictValue& action,
                                        std::string_view action_type,
                                        ToolCallback& done) {
  std::optional<int> index = OptionalElementIndex(action);
  if (!index.has_value()) {
    std::move(done).Reject(
        "invalid_args",
        base::StrCat({action_type,
                      ": missing required `index` action parameter"}));
  }
  return index;
}

std::optional<int> RequiredIntArg(const base::DictValue& action,
                                  std::string_view action_type,
                                  std::string_view name,
                                  ToolCallback& done) {
  std::optional<int> value = action.FindInt(name);
  if (!value.has_value()) {
    std::move(done).Reject(
        "invalid_args",
        base::StrCat({action_type, ": missing required `", name, "`"}));
  }
  return value;
}

std::optional<std::string> RequiredStringArg(const base::DictValue& action,
                                             std::string_view action_type,
                                             std::string_view name,
                                             ToolCallback& done) {
  const std::string* value = action.FindString(name);
  if (!value || value->empty()) {
    std::move(done).Reject(
        "invalid_args",
        base::StrCat({action_type, ": missing required `", name, "`"}));
    return std::nullopt;
  }
  return *value;
}

std::optional<std::string> OptionalStringArg(const base::DictValue& action,
                                             std::string_view name) {
  const std::string* value = action.FindString(name);
  if (!value || value->empty()) {
    return std::nullopt;
  }
  return *value;
}

std::optional<bool> OptionalBoolArg(const base::DictValue& action,
                                    std::string_view name) {
  return action.FindBool(name);
}

bool ActionTypeTerminatesSequence(std::string_view action_type) {
  return action_type == "done" || action_type == "navigate" ||
         action_type == "search" || action_type == "back" ||
         action_type == "forward" || action_type == "create_tab" ||
         action_type == "close_tab" || action_type == "activate_tab";
}

std::optional<base::DictValue> ExtractWorkedTabObservation(
    base::Value& result,
    std::optional<int> preferred_tab_id) {
  if (!result.is_dict()) {
    return std::nullopt;
  }

  base::DictValue& result_dict = result.GetDict();
  const base::DictValue* observation = result_dict.FindDict("observation");
  if (!observation) {
    return std::nullopt;
  }

  const base::ListValue* tabs = observation->FindList("tabs");
  if (!tabs) {
    result_dict.Remove("observation");
    return std::nullopt;
  }

  if (preferred_tab_id.has_value()) {
    for (const base::Value& tab : *tabs) {
      if (!tab.is_dict()) {
        continue;
      }
      const base::DictValue& tab_dict = tab.GetDict();
      if (tab_dict.FindInt("tab_id") == preferred_tab_id) {
        base::DictValue worked_tab = tab_dict.Clone();
        result_dict.Remove("observation");
        return worked_tab;
      }
    }
  }

  for (const base::Value& tab : *tabs) {
    if (tab.is_dict()) {
      base::DictValue worked_tab = tab.GetDict().Clone();
      result_dict.Remove("observation");
      return worked_tab;
    }
  }

  result_dict.Remove("observation");
  return std::nullopt;
}

std::optional<int> PreferredObservationTabIdForAction(
    const base::DictValue& action,
    std::string_view action_type) {
  if (action_type == "activate_tab" || action_type == "close_tab" ||
      action_type == "navigate" || action_type == "wait" ||
      action_type == "screenshot" || action_type == "read_page" ||
      action_type == "find_text" || action_type == "media_play" ||
      action_type == "media_pause" || action_type == "media_seek" ||
      action_type == "media_state") {
    return action.FindInt("tab_id");
  }
  return OptionalIntArg(action, "tab_id");
}

// Lightweight host-path extractor used only for action summaries. We
// deliberately don't pull in GURL here — `long_term_memory` is a 200-byte
// human-readable hint, not an authoritative URL parser. Returns
// "host[/first_path]" capped at 60 chars.
std::string HostPathForSummary(std::string_view url) {
  if (url.empty()) {
    return std::string();
  }
  size_t scheme_end = url.find("://");
  size_t start = (scheme_end != std::string_view::npos) ? scheme_end + 3 : 0;
  size_t end = url.size();
  for (char c : {'?', '#'}) {
    size_t idx = url.find(c, start);
    if (idx != std::string_view::npos && idx < end) {
      end = idx;
    }
  }
  // Allow one path segment past the host so e.g. "google.com/search"
  // distinguishes search results from a homepage hit.
  size_t slash = url.find('/', start);
  if (slash != std::string_view::npos && slash < end) {
    size_t next_slash = url.find('/', slash + 1);
    if (next_slash != std::string_view::npos && next_slash < end) {
      end = next_slash;
    }
  }
  if (start >= end) {
    return std::string(url.substr(0, std::min<size_t>(60, url.size())));
  }
  std::string host_path(url.substr(start, end - start));
  if (host_path.size() > 60) {
    host_path.resize(60);
  }
  return host_path;
}

// Builds a one-line summary of an action's input + outcome for the
// model's long-term memory. Mirrors browser-use's
// `ActionResult.long_term_memory` mechanism — survives across history
// turns even after the bulky observation gets stripped, so the model
// retains a navigational diary of "what I've already done" rather than
// a sequence of opaque {tab_id, note} placeholders.
//
// Returns ≤200 bytes UTF-8-safe. `result` may be a dict with action-
// specific fields (page_text length, matches[], etc.) — only read for
// actions that produce a meaningful payload (read_page, read_element,
// find_text); other actions get summarized purely from their input.
std::string BuildLongTermMemoryForAction(std::string_view action_type,
                                         const base::DictValue& action,
                                         const base::Value& result) {
  constexpr int kLtmMaxBytes = 200;
  constexpr int kLtmTextSnippetMax = 40;

  auto str_arg = [&](std::string_view key) -> std::string {
    const std::string* p = action.FindString(key);
    return p ? *p : std::string();
  };
  auto int_arg = [&](std::string_view key) -> std::optional<int> {
    return action.FindInt(key);
  };
  auto trim = [](std::string s, int n) -> std::string {
    return TruncateUtf8Snippet(std::move(s), n);
  };

  const base::DictValue* rdict =
      result.is_dict() ? &result.GetDict() : nullptr;

  std::string out;

  if (action_type == "search") {
    std::string engine = str_arg("engine");
    if (engine.empty()) {
      engine = "Google";
    }
    out = base::StrCat({"Searched ", engine, " for '",
                        trim(str_arg("query"), 60), "'"});
  } else if (action_type == "navigate") {
    out = base::StrCat({"Navigated to ", HostPathForSummary(str_arg("url"))});
    if (auto tab = int_arg("tab_id")) {
      out =
          base::StrCat({out, " (tab ", base::NumberToString(*tab), ")"});
    }
  } else if (action_type == "click") {
    auto i = int_arg("index");
    out = base::StrCat({"Clicked element ",
                        i ? base::NumberToString(*i) : "?"});
    std::string button = str_arg("button");
    if (!button.empty() && button != "left") {
      out = base::StrCat({out, " (", button, ")"});
    }
    std::string clicks = str_arg("click_count");
    if (clicks == "double") {
      out = base::StrCat({out, " (double)"});
    }
  } else if (action_type == "input") {
    auto i = int_arg("index");
    out = base::StrCat({"Typed '", trim(str_arg("text"), kLtmTextSnippetMax),
                        "' into ", i ? base::NumberToString(*i) : "?"});
    if (action.FindBool("submit").value_or(false)) {
      out = base::StrCat({out, " (submit)"});
    }
  } else if (action_type == "select") {
    auto i = int_arg("index");
    out = base::StrCat({"Selected '", trim(str_arg("value"), kLtmTextSnippetMax),
                        "' on ", i ? base::NumberToString(*i) : "?"});
  } else if (action_type == "hover") {
    auto i = int_arg("index");
    out = base::StrCat({"Hovered element ",
                        i ? base::NumberToString(*i) : "?"});
  } else if (action_type == "drag_and_drop") {
    auto f = int_arg("from_index");
    auto t = int_arg("to_index");
    out = base::StrCat({"Dragged ", f ? base::NumberToString(*f) : "?",
                        " to ", t ? base::NumberToString(*t) : "?"});
  } else if (action_type == "scroll") {
    std::string direction = str_arg("direction");
    if (direction.empty()) {
      direction = "down";
    }
    out = base::StrCat({"Scrolled ", direction});
    if (auto distance = action.FindDouble("distance")) {
      out = base::StrCat({out, " ",
                          base::NumberToString(static_cast<int>(*distance)),
                          "px"});
    }
  } else if (action_type == "scroll_to") {
    auto i = int_arg("index");
    out = base::StrCat({"Scrolled to element ",
                        i ? base::NumberToString(*i) : "?"});
  } else if (action_type == "wait") {
    auto ms = int_arg("wait_ms");
    out = base::StrCat({"Waited ",
                        ms ? base::NumberToString(*ms) : "default", "ms"});
  } else if (action_type == "back") {
    out = "Navigated back";
  } else if (action_type == "forward") {
    out = "Navigated forward";
  } else if (action_type == "create_tab") {
    out = base::StrCat({"Created tab to ",
                        HostPathForSummary(str_arg("url"))});
  } else if (action_type == "close_tab") {
    auto t = int_arg("tab_id");
    out = t ? base::StrCat({"Closed tab ", base::NumberToString(*t)})
            : "Closed tab";
  } else if (action_type == "activate_tab") {
    auto t = int_arg("tab_id");
    out = t ? base::StrCat({"Activated tab ", base::NumberToString(*t)})
            : "Activated tab";
  } else if (action_type == "media_play") {
    out = "Played media";
  } else if (action_type == "media_pause") {
    out = "Paused media";
  } else if (action_type == "media_seek") {
    auto ms = int_arg("seek_time_ms");
    out = base::StrCat({"Seeked media to ",
                        ms ? base::NumberToString(*ms) : "0", "ms"});
  } else if (action_type == "media_state") {
    out = "Read media state";
  } else if (action_type == "screenshot") {
    out = "Took screenshot";
  } else if (action_type == "read_page") {
    int chars = 0;
    std::string title;
    std::string url;
    if (rdict) {
      if (const std::string* p = rdict->FindString("page_text")) {
        chars = static_cast<int>(p->size());
      }
      if (const std::string* p = rdict->FindString("title")) {
        title = *p;
      }
      if (const std::string* p = rdict->FindString("url")) {
        url = *p;
      }
    }
    std::string host_path = HostPathForSummary(url);
    out = "Read page";
    if (!host_path.empty()) {
      out = base::StrCat({out, " ", host_path});
    }
    if (!title.empty()) {
      out = base::StrCat({out, " ('", trim(title, kLtmTextSnippetMax), "')"});
    }
    out = base::StrCat({out, " ", base::NumberToString(chars), " chars"});
  } else if (action_type == "read_element") {
    int chars = 0;
    std::string type;
    auto i = int_arg("index");
    if (rdict) {
      if (const std::string* p = rdict->FindString("element_text")) {
        chars = static_cast<int>(p->size());
      }
      if (const std::string* p = rdict->FindString("type")) {
        type = *p;
      }
    }
    out = base::StrCat({"Read element ",
                        i ? base::NumberToString(*i) : "?"});
    if (!type.empty()) {
      out = base::StrCat({out, " (", type, ")"});
    }
    out = base::StrCat({out, " ", base::NumberToString(chars), " chars"});
  } else if (action_type == "find_text") {
    int matches_count = 0;
    if (rdict) {
      if (const base::ListValue* m = rdict->FindList("matches")) {
        matches_count = static_cast<int>(m->size());
      }
    }
    out = base::StrCat({"Found '", trim(str_arg("query"), kLtmTextSnippetMax),
                        "' (", base::NumberToString(matches_count),
                        " matches)"});
  } else if (action_type == "done") {
    bool success = action.FindBool("success").value_or(false);
    std::string text = trim(str_arg("text"), 80);
    out = base::StrCat({success ? "Finished: " : "Aborted: ",
                        text.empty() ? "(no text)" : text});
  } else {
    out = base::StrCat({"Performed ", action_type});
  }

  return TruncateUtf8Snippet(std::move(out), kLtmMaxBytes);
}

// Top-level summary cap. Joined per-action pieces are clipped here so a
// 5-action batch doesn't blow past ~1KB of compaction-resistant memory.
constexpr int kLtmTopLevelMaxBytes = 800;

// Joins per-action long_term_memory pieces with " | " and stamps the
// capped result onto `out["long_term_memory"]`. Skipped silently when
// no pieces accumulated (e.g. the batch failed before any action ran)
// so the resulting JSON simply lacks the field.
void StampJoinedLongTermMemory(base::DictValue& out,
                               const std::vector<std::string>& pieces) {
  if (pieces.empty()) {
    return;
  }
  std::string joined = base::JoinString(pieces, " | ");
  out.Set("long_term_memory",
          TruncateUtf8Snippet(std::move(joined), kLtmTopLevelMaxBytes));
}

}  // namespace

struct BrowserTool::BrowserActionBatchState
    : public base::RefCounted<BrowserActionBatchState> {
  BrowserActionBatchState(std::vector<base::DictValue> actions,
                          std::optional<std::string> evaluation_previous_goal,
                          std::optional<std::string> memory,
                          std::optional<std::string> next_goal,
                          ToolCallback done)
      : actions(std::move(actions)),
        evaluation_previous_goal(std::move(evaluation_previous_goal)),
        memory(std::move(memory)),
        next_goal(std::move(next_goal)),
        done(std::move(done)) {}

  std::vector<base::DictValue> actions;
  base::ListValue results;
  std::optional<base::DictValue> observation;
  scoped_refptr<PausableTimeout> step_timeout;
  bool timed_out = false;
  // Reflection fields (browser-use mechanism #1). The schema asks the
  // model to fill these per-call as a self-tracking scratchpad. They
  // already round-trip through the assistant `tool_use.input` block, but
  // we ALSO echo them onto the result dict — see ResolveBrowserActions —
  // so the model has its own past intent visible alongside the action
  // outcomes. Together with `memory_pieces` (mechanism #2) this gives
  // the model a unified per-step diary that survives microcompaction.
  std::optional<std::string> evaluation_previous_goal;
  std::optional<std::string> memory;
  std::optional<std::string> next_goal;
  // Per-action long_term_memory pieces (browser-use mechanism #2).
  // Each entry is a 1-line summary built by BuildLongTermMemoryForAction
  // from the action's input + outcome. At resolve time we join with
  // " | " and stamp onto the result dict's top-level `long_term_memory`
  // field so the model gets a navigational diary that survives
  // microcompaction even after `observation.elements[]` is stripped.
  std::vector<std::string> memory_pieces;
  // Soft-recovery diagnostic. When the model emits a malformed call
  // (missing `actions` key, or empty `actions[]`) we synthesize a
  // no-op `wait` so the call still returns a fresh observation
  // instead of erroring. The variant tells `ResolveBrowserActions`
  // which warning text to surface so the model learns the specific
  // mistake.
  enum class WaitSynthesisCause {
    kNone,
    kActionsKeyMissing,  // args was `{}` or omitted `actions` entirely.
    kActionsArrayEmpty,  // model sent `actions: []` explicitly.
  };
  WaitSynthesisCause wait_synthesis_cause = WaitSynthesisCause::kNone;
  ToolCallback done;

 private:
  friend class base::RefCounted<BrowserActionBatchState>;
  ~BrowserActionBatchState() = default;
};

void BrowserTool::browser_actions(
    std::optional<std::string> evaluation_previous_goal,
    std::optional<std::string> memory,
    std::optional<std::string> next_goal,
    std::optional<base::ListValue> actions_opt,
    ToolCallback done) {
  // Reflection fields are advisory bookkeeping for the model
  // (browser-use mechanism #1 — self-report). They are required by
  // the schema, but extraction stays lenient (std::optional<std::string>)
  // so a missing reflection field never causes an `invalid_args`
  // rejection. We capture them onto BrowserActionBatchState and echo
  // them back in the resolved result dict (see ResolveBrowserActions),
  // so the model's own past intent stays visible alongside the action
  // outcomes — together with the `long_term_memory` action diary
  // (mechanism #2) this gives the model a unified per-step record
  // that survives microcompaction.

  // Malformed-call rejection. `actions` is required by contract; we
  // still accept the key as optional at the extraction layer (so a
  // model emitting `args:{}` doesn't get an `invalid_args` reject
  // before our handler runs) but we no longer synthesize a recovery
  // `wait`.
  //
  // Why no synthetic wait: appending a wait gave the call a fresh
  // observation, and the model would attend to the new page state
  // instead of the error envelope — exactly the failure mode the
  // 2026-05-07 Haiku 4.5 logs caught (5 consecutive `args:{}` calls
  // before MAX_FAILURES tripped). With the JS dispatch site now
  // setting `isError: true` on the canonical tool_result block (see
  // nexus_web/src/services/tools/toolExecution.ts), the Anthropic
  // wire layer emits `is_error: true`, which triggers Anthropic's
  // documented 2-3x retry-with-corrections behaviour for the model.
  // Bundling an observation with the error dilutes that signal —
  // return ONLY the error envelope so the failure is unambiguous.
  // OpenAI/Google/Ollama don't have a protocol-level error flag but
  // still see the `ok:false` + structured `error.{code,message,
  // remediation}` envelope at the top of the result dict.
  using WaitCause = BrowserActionBatchState::WaitSynthesisCause;
  WaitCause wait_cause = WaitCause::kNone;
  base::ListValue actions;
  if (!actions_opt.has_value()) {
    wait_cause = WaitCause::kActionsKeyMissing;
  } else {
    actions = std::move(*actions_opt);
    if (actions.empty()) {
      wait_cause = WaitCause::kActionsArrayEmpty;
    }
  }
  if (wait_cause != WaitCause::kNone) {
    // Build the error envelope inline and resolve immediately. No
    // observation, no results, no synthetic action — just the failure
    // signal plus echoed reflection fields (so the diary stays
    // truthful about what the model itself wrote on this turn).
    base::DictValue result;
    result.Set("action", "browser_actions");
    if (evaluation_previous_goal.has_value() &&
        !evaluation_previous_goal->empty()) {
      result.Set("evaluation_previous_goal", *evaluation_previous_goal);
    }
    if (memory.has_value() && !memory->empty()) {
      result.Set("memory", *memory);
    }
    if (next_goal.has_value() && !next_goal->empty()) {
      result.Set("next_goal", *next_goal);
    }

    std::string error_code;
    std::string error_message;
    std::string error_remediation;
    std::string ltm;
    if (wait_cause == WaitCause::kActionsKeyMissing) {
      error_code = "MISSING_ACTIONS_KEY";
      error_message =
          "Your tool call had no `actions` key (e.g. args was `{}`). "
          "`actions` is REQUIRED — send 1-5 action objects per call.";
      error_remediation =
          "Retry with ALL required fields populated: "
          "`evaluation_previous_goal`, `memory`, `next_goal`, and "
          "`actions` (1-5 items). Each action object needs a `type` "
          "field plus that type's params — e.g. "
          "[{type:\"click\",index:N}], [{type:\"navigate\",url:\"...\"}], "
          "or [{type:\"done\",success:true,text:\"...\"}].";
      ltm = "REJECTED: malformed call (no `actions` key); no action run";
    } else {  // kActionsArrayEmpty
      error_code = "EMPTY_ACTIONS_ARRAY";
      error_message =
          "Your tool call had `actions: []` (empty array). `actions` "
          "is REQUIRED to contain 1-5 action objects.";
      error_remediation =
          "Retry with at least one action object inside `actions`. "
          "Each item needs a `type` field plus that type's params.";
      ltm = "REJECTED: malformed call (empty `actions: []`); no action run";
    }
    result.Set("long_term_memory", ltm);
    result.Set("ok", false);

    base::DictValue error_dict;
    error_dict.Set("code", error_code);
    error_dict.Set("message", error_message);
    error_dict.Set("remediation", error_remediation);
    result.Set("error", std::move(error_dict));

    std::move(done).Resolve(base::Value(std::move(result)));
    return;
  }
  if (actions.size() > kMaxBrowserActionsPerStep) {
    std::move(done).Reject(
        "invalid_args",
        "browser_actions received too many actions; max_actions_per_step is 5");
    return;
  }

  std::vector<base::DictValue> action_items;
  action_items.reserve(actions.size());
  for (const base::Value& item : actions) {
    if (!item.is_dict()) {
      std::move(done).Reject(
          "invalid_args",
          "browser_actions actions[] items must be objects with a type field");
      return;
    }
    base::DictValue action = item.GetDict().Clone();
    const std::string* type = action.FindString("type");
    if (!type || type->empty()) {
      std::move(done).Reject(
          "invalid_args",
          "browser_actions actions[] items must include non-empty `type`");
      return;
    }
    action_items.push_back(std::move(action));
  }

  auto state = base::MakeRefCounted<BrowserActionBatchState>(
      std::move(action_items), std::move(evaluation_previous_goal),
      std::move(memory), std::move(next_goal), std::move(done));
  state->wait_synthesis_cause = wait_cause;
  state->step_timeout = RegisterPausableTimeout(
      step_timeout_, "browser_actions",
      base::BindOnce(&BrowserTool::OnBrowserActionsStepTimeout,
                     weak_ptr_factory_.GetWeakPtr(), state));
  RunBrowserActionBatch(std::move(state), /*index=*/0);
}

void BrowserTool::ResolveBrowserActions(
    scoped_refptr<BrowserActionBatchState> state,
    base::DictValue result) {
  if (state->step_timeout) {
    state->step_timeout->Stop();
  }
  if (state->done.fired()) {
    return;
  }

  // ---- Mechanism #1 + #2 metadata stamping (browser-use port) ----------
  // Echo the model-supplied reflection fields (mechanism #1) and stamp
  // the joined per-action diary (mechanism #2). Both happen here so
  // every resolution path — success, error, step timeout, malformed
  // call — produces a uniformly enriched result dict.
  //
  // Why echo mechanism #1 fields onto the result:
  // they already round-trip through the assistant `tool_use.input`, but
  // mirroring them onto the result makes the on-disk tool-details JSON
  // self-contained for UI replay AND keeps them adjacent to the action
  // outcomes in the model's view. Cheap (≤ a few hundred bytes).
  // Skip empty strings: schema marks these required, but extraction stays
  // lenient (std::optional<std::string>) so a model that drops one still
  // delivers `nullopt`. A model that DOES send the field but leaves it
  // empty (`""`) would round-trip a useless empty echo into history;
  // suppress those too so the history stays meaningful.
  if (state->evaluation_previous_goal.has_value() &&
      !state->evaluation_previous_goal->empty()) {
    result.Set("evaluation_previous_goal", *state->evaluation_previous_goal);
  }
  if (state->memory.has_value() && !state->memory->empty()) {
    result.Set("memory", *state->memory);
  }
  if (state->next_goal.has_value() && !state->next_goal->empty()) {
    result.Set("next_goal", *state->next_goal);
  }
  StampJoinedLongTermMemory(result, state->memory_pieces);

  // ---- Malformed-call recovery (kActionsKeyMissing / kActionsArrayEmpty)
  // Surface a soft warning to the LLM when we recovered from a
  // malformed call by synthesizing a `wait`.
  //
  // Format choice (cross-provider portable): emit a structured
  // `error: {code, message, remediation}` field at the TOP of the
  // result dict — NOT a `<system-reminder>` text prefix.
  //
  // Reasoning (revised after multi-provider audit):
  //   1. `<system-reminder>` is a Claude-specific trained signal. For
  //      OpenAI / Gemini / Qwen / Kimi / GLM the tag is plain text
  //      with no special attention boost — relying on it makes the
  //      recovery path effectively Claude-only.
  //   2. A `{ok:false, error:{code,message,remediation}}` shape is
  //      the standard REST/RPC error envelope (RFC 7807-ish). Any
  //      model that has seen JSON API error patterns in training
  //      will recognize and attend to it, regardless of provider.
  //   3. JSON keys are alphabetically inserted by base::DictValue's
  //      writer, so `error` lands at "e" — BEFORE `observation`
  //      ("o") and BEFORE `results` ("r"). The warning therefore
  //      sits in the primacy slot the model reads first, without
  //      needing a text prefix to manufacture position.
  //   4. The result stays a dict, so consumers (microcompact,
  //      query.ts soft-failure detector, UI replay) can JSON.parse
  //      it without first stripping a text prefix. The previous
  //      string-with-prefix shape silently broke the soft-failure
  //      detector in query.ts (JSON.parse threw on `<`, catch
  //      returned false, consecutiveFailures never incremented,
  //      maxFailures circuit never tripped — see 2026-05-04 stuck-
  //      loop analysis with 24 consecutive args:{} calls).
  //
  // The system prompt teaches the model to read `error.*` fields
  // first when `ok:false` (see systemPromptToolSection.ts).
  using WaitCause = BrowserActionBatchState::WaitSynthesisCause;
  if (state->wait_synthesis_cause != WaitCause::kNone) {
    std::string error_code;
    std::string error_message;
    std::string error_remediation;
    std::string ltm;
    if (state->wait_synthesis_cause == WaitCause::kActionsKeyMissing) {
      error_code = "MISSING_ACTIONS_KEY";
      error_message =
          "Your tool call had no `actions` key (e.g. args was `{}`). "
          "`actions` is REQUIRED — send 1-5 action objects per call.";
      error_remediation =
          "Synthesized [{type:\"wait\"}] so this call returned a fresh "
          "observation. Next turn send concrete actions: e.g. "
          "[{type:\"click\",index:N}], [{type:\"navigate\",url:\"...\"}], "
          "or [{type:\"done\",success:true,text:\"...\"}]. Do NOT repeat "
          "an empty call — it cannot make progress.";
      ltm = "REJECTED: empty actions[]; synthesized a wait";
    } else {  // kActionsArrayEmpty
      error_code = "EMPTY_ACTIONS_ARRAY";
      error_message =
          "Your tool call had `actions: []` (empty array). `actions` is "
          "REQUIRED to contain 1-5 action objects.";
      error_remediation =
          "Synthesized [{type:\"wait\"}] so this call returned a fresh "
          "observation. Next turn send concrete actions instead of an "
          "empty array.";
      ltm = "REJECTED: actions: [] empty array; synthesized a wait";
    }

    // Override the long_term_memory diary entry: the synthesized
    // `wait` would otherwise show up as "Waited 100ms" in history, which
    // is misleading — the actual story is the model sent a malformed
    // call. Replace with a rejection summary so the historical diary
    // honestly reflects what happened.
    result.Set("long_term_memory", ltm);

    // The malformed call is NOT a success — flip `ok` to false so the
    // model sees this turn as a failure (matching what actually
    // happened: it sent a malformed call and got a recovery
    // observation, not a real action result). Observation, results,
    // stop_reason, tab_changed, etc. stay in the JSON body so the
    // model still has the context it needs to recover.
    result.Set("ok", false);
    // Drop any pre-existing warnings field — superseded by the
    // structured `error` field stamped below.
    result.Remove("warnings");

    base::DictValue error_dict;
    error_dict.Set("code", error_code);
    error_dict.Set("message", error_message);
    error_dict.Set("remediation", error_remediation);
    result.Set("error", std::move(error_dict));

    std::move(state->done).Resolve(base::Value(std::move(result)));
    return;
  }
  std::move(state->done).Resolve(std::move(result));
}

void BrowserTool::OnBrowserActionsStepTimeout(
    scoped_refptr<BrowserActionBatchState> state) {
  if (state->done.fired()) {
    return;
  }
  state->timed_out = true;
  StopCurrentActorTaskForTimeout("browser_actions_step_timeout");

  // Push a step-timeout summary onto the action diary so history
  // honestly reflects "this batch hit the wall-clock limit" instead of
  // dropping into a black-hole observation strip.
  state->memory_pieces.push_back(base::StrCat(
      {"step_timeout after ",
       base::NumberToString(step_timeout_.InMilliseconds()), "ms"}));

  base::DictValue out;
  out.Set("ok", false);
  out.Set("action", "browser_actions");
  out.Set("stop_reason", "step_timeout");
  out.Set("timed_out", true);
  if (state->observation.has_value()) {
    out.Set("observation", state->observation->Clone());
  }
  out.Set("results", state->results.Clone());
  ResolveBrowserActions(std::move(state), std::move(out));
}

void BrowserTool::RunBrowserActionBatch(
    scoped_refptr<BrowserActionBatchState> state,
    size_t index) {
  if (index >= state->actions.size()) {
    base::DictValue out;
    out.Set("ok", true);
    out.Set("action", "browser_actions");
    if (state->observation.has_value()) {
      out.Set("observation", std::move(*state->observation));
    }
    out.Set("results", std::move(state->results));
    ResolveBrowserActions(std::move(state), std::move(out));
    return;
  }

  if (state->timed_out || state->done.fired()) {
    return;
  }

  base::DictValue action = state->actions[index].Clone();
  std::string action_type = *action.FindString("type");
  std::optional<int> preferred_tab_id =
      PreferredObservationTabIdForAction(action, action_type);
  // Fall back to the active tab when the LLM omitted `tab_id`. Without
  // this, `ExtractWorkedTabObservation` falls back to "first tab in
  // result_proto.tabs()", which is non-deterministic — see the comment
  // in `BuildObservationDict` on the upstream
  // `GetLastActedTabs()`/flat_hash_set ordering bug. Pinning the
  // worked-tab pick to the active tab makes per-batch observations
  // consistently surface the user's foreground tab whenever the
  // action didn't specify a target.
  if (!preferred_tab_id.has_value()) {
    if (std::optional<int32_t> active =
            GetActiveTabHandleId(GetProfile())) {
      preferred_tab_id = *active;
    }
  }
  const std::optional<int> pre_action_tab_id = last_observation_tab_id_;
  const std::string pre_action_url = last_observation_url_;
  const bool pre_action_focus_present = last_observation_focus_target_present_;
  const std::string pre_action_focus_target_id =
      last_observation_focus_target_id_;

  ToolCallback item_done(
      base::BindOnce(&BrowserTool::OnBrowserActionItemSuccess,
                     weak_ptr_factory_.GetWeakPtr(), state, index, action_type,
                     preferred_tab_id, pre_action_tab_id,
                     pre_action_url, pre_action_focus_present,
                     pre_action_focus_target_id),
      base::BindOnce(&BrowserTool::OnBrowserActionItemError,
                     weak_ptr_factory_.GetWeakPtr(), state, index,
                     action_type));
  RunBrowserActionItem(action, action_type, std::move(item_done));
}

void BrowserTool::OnBrowserActionItemSuccess(
    scoped_refptr<BrowserActionBatchState> state,
    size_t index,
    std::string action_type,
    std::optional<int> preferred_tab_id,
    std::optional<int> pre_action_tab_id,
    std::string pre_action_url,
    bool pre_action_focus_present,
    std::string pre_action_focus_target_id,
    base::Value result) {
  if (state->timed_out || state->done.fired()) {
    return;
  }
  if (std::optional<base::DictValue> observation =
          ExtractWorkedTabObservation(result, preferred_tab_id)) {
    state->observation = std::move(*observation);
  }

  // Anti-blind-fold: ensure the observation always carries `url` and
  // `title` for the primary tab, even when ExtractWorkedTabObservation
  // returned a thin tab dict (just `tab_id`) or returned nullopt entirely.
  //
  // Background: actions that trigger url_changed (click into a link,
  // navigate) often complete before the post-navigation page generates
  // annotated_page_content. BuildObservationDict therefore writes only
  // `tab_id` into that tab's dict (no url, no title, no elements). The
  // microcompact strip later treats the result as already-minimal and
  // emits `{tab_id, summary: "Clicked element N"}` to the model — which
  // tells the model what it DID but NOT where it landed. The model
  // can't decide `back` vs `retry` vs `done` without that signal and
  // tends to drift into text-only thrashing (see 2026-05-04 stuck-loop
  // analysis after click-to-Aiqicha).
  //
  // `last_observation_url_` / `last_observation_title_` were just
  // populated by BuildObservationDict from the primary tab. They reflect
  // the post-action page, so they're the right fallback. If
  // BuildObservationDict didn't run this batch (no result tabs, very
  // rare), they hold the previous batch's values — still better than
  // an empty observation, since the model at least knows the last
  // page it was on.
  if (!last_observation_url_.empty() || !last_observation_title_.empty() ||
      last_observation_tab_id_.has_value()) {
    if (!state->observation.has_value()) {
      base::DictValue obs;
      if (last_observation_tab_id_.has_value()) {
        obs.Set("tab_id", *last_observation_tab_id_);
      }
      state->observation = std::move(obs);
    }
    base::DictValue& obs = *state->observation;
    if (!obs.FindInt("tab_id") && last_observation_tab_id_.has_value()) {
      obs.Set("tab_id", *last_observation_tab_id_);
    }
    if (!obs.contains("url") && !last_observation_url_.empty()) {
      obs.Set("url", last_observation_url_);
    }
    if (!obs.contains("title") && !last_observation_title_.empty()) {
      obs.Set("title", last_observation_title_);
    }
  }

  // Compute per-action long_term_memory BEFORE moving `result` into the
  // wrapped entry. The summary captures the action's input + outcome
  // and survives history compaction (browser-use mechanism #2).
  //
  // Defensive: `index` is bounded by the caller (see
  // `RunBrowserActionBatch` which checks `index >= state->actions.size()`
  // before scheduling this callback), but verify it explicitly here so a
  // future refactor that loosens the caller's contract can't silently
  // turn this into UB. On out-of-range, fall back to an empty action
  // dict — the per-action summary is best-effort, not load-bearing.
  // Stack-local (not `static const`) because Chromium's
  // -Wexit-time-destructors policy bans static-storage objects with
  // non-trivial destructors; a default-constructed DictValue is cheap.
  const base::DictValue empty_action;
  const base::DictValue& action_for_summary =
      index < state->actions.size() ? state->actions[index] : empty_action;
  std::string ltm = BuildLongTermMemoryForAction(
      action_type, action_for_summary, result);
  state->memory_pieces.push_back(ltm);

  base::DictValue wrapped_result;
  wrapped_result.Set("action", action_type);
  // Surface the per-action summary on the wrapped entry as well, so the
  // microcompact synthesis fallback (`results[].long_term_memory` →
  // joined top-level when the top-level field is missing) still works
  // on legacy/partial records.
  wrapped_result.Set("long_term_memory", ltm);
  wrapped_result.Set("result", std::move(result));
  state->results.Append(std::move(wrapped_result));

  const bool url_changed = !pre_action_url.empty() &&
                           !last_observation_url_.empty() &&
                           pre_action_url != last_observation_url_;
  const bool focus_changed =
      pre_action_focus_present != last_observation_focus_target_present_ ||
      (pre_action_focus_present && last_observation_focus_target_present_ &&
       pre_action_focus_target_id != last_observation_focus_target_id_);
  std::optional<int> post_action_tab_id = last_observation_tab_id_;
  if (state->observation.has_value()) {
    if (std::optional<int> observed_tab_id =
            state->observation->FindInt("tab_id")) {
      post_action_tab_id = observed_tab_id;
    }
  }
  const bool tab_changed = pre_action_tab_id.has_value() &&
                           post_action_tab_id.has_value() &&
                           pre_action_tab_id != post_action_tab_id;
  const bool should_stop =
      ActionTypeTerminatesSequence(action_type) || tab_changed || url_changed ||
      focus_changed;

  if (should_stop || index + 1 >= state->actions.size()) {
    base::DictValue out;
    out.Set("ok", true);
    out.Set("action", "browser_actions");
    out.Set("stopped_after_action_index", static_cast<int>(index));
    out.Set("tab_changed", tab_changed);
    if (pre_action_tab_id.has_value()) {
      out.Set("pre_action_tab_id", *pre_action_tab_id);
    }
    if (post_action_tab_id.has_value()) {
      out.Set("post_action_tab_id", *post_action_tab_id);
    }
    out.Set("url_changed", url_changed);
    out.Set("focus_changed", focus_changed);
    if (tab_changed) {
      out.Set("stop_reason", "tab_changed");
    } else if (url_changed) {
      out.Set("stop_reason", "url_changed");
    } else if (focus_changed) {
      out.Set("stop_reason", "focus_changed");
    } else if (ActionTypeTerminatesSequence(action_type)) {
      out.Set("stop_reason", "sequence_terminator");
    } else {
      out.Set("stop_reason", "complete");
    }
    if (state->observation.has_value()) {
      out.Set("observation", std::move(*state->observation));
    }
    out.Set("results", std::move(state->results));
    ResolveBrowserActions(std::move(state), std::move(out));
    return;
  }

  RunBrowserActionBatch(std::move(state), index + 1);
}

void BrowserTool::OnBrowserActionItemError(
    scoped_refptr<BrowserActionBatchState> state,
    size_t index,
    std::string action_type,
    std::string code,
    std::string message) {
  if (state->timed_out || state->done.fired()) {
    return;
  }
  // Push a failure summary onto the action diary so history shows
  // which action failed and why. Capped at 200 bytes via the same
  // helper used for success-path summaries.
  state->memory_pieces.push_back(TruncateUtf8Snippet(
      base::StrCat({"Action ", action_type, " failed: ", code}), 200));

  base::DictValue error;
  error.Set("ok", false);
  error.Set("action", action_type);
  error.Set("result", code);
  error.Set("error", message);
  // Mirror the per-action long_term_memory onto the failed entry so
  // microcompact's results[].long_term_memory fallback path picks it
  // up (matches the success path).
  error.Set(
      "long_term_memory",
      TruncateUtf8Snippet(
          base::StrCat({"Action ", action_type, " failed: ", code}), 200));
  state->results.Append(std::move(error));

  base::DictValue out;
  out.Set("ok", false);
  out.Set("action", "browser_actions");
  out.Set("stopped_after_action_index", static_cast<int>(index));
  out.Set("stop_reason", "error");
  if (state->observation.has_value()) {
    out.Set("observation", std::move(*state->observation));
  }
  out.Set("results", std::move(state->results));
  ResolveBrowserActions(std::move(state), std::move(out));
}

void BrowserTool::RunBrowserActionItem(const base::DictValue& action,
                                       std::string_view action_type,
                                       ToolCallback done) {
  if (action_type == "done") {
    base::DictValue result;
    result.Set("ok", true);
    result.Set("is_done", true);
    result.Set("success", action.FindBool("success").value_or(false));
    if (const std::string* text = action.FindString("text")) {
      result.Set("text", *text);
    }
    std::move(done).Resolve(std::move(result));
    return;
  }

  if (action_type == "click") {
    std::optional<int> index = RequiredElementIndex(action, action_type, done);
    if (!index.has_value()) {
      return;
    }
    RunClickAction(*index, OptionalIntArg(action, "tab_id"),
                   OptionalStringArg(action, "button"),
                   OptionalStringArg(action, "click_count"), std::move(done));
    return;
  }

  if (action_type == "input") {
    std::optional<int> index = RequiredElementIndex(action, action_type, done);
    if (!index.has_value()) {
      return;
    }
    std::optional<std::string> text =
        RequiredStringArg(action, action_type, "text", done);
    if (!text.has_value()) {
      return;
    }
    RunInputAction(*index, OptionalIntArg(action, "tab_id"), std::move(*text),
                   OptionalBoolArg(action, "submit"), std::move(done));
    return;
  }

  if (action_type == "select") {
    std::optional<int> index = RequiredElementIndex(action, action_type, done);
    if (!index.has_value()) {
      return;
    }
    std::optional<std::string> value =
        RequiredStringArg(action, action_type, "value", done);
    if (!value.has_value()) {
      return;
    }
    RunSelectAction(*index, OptionalIntArg(action, "tab_id"),
                    std::move(*value), std::move(done));
    return;
  }

  if (action_type == "hover") {
    std::optional<int> index = RequiredElementIndex(action, action_type, done);
    if (!index.has_value()) {
      return;
    }
    RunHoverAction(*index, OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }

  if (action_type == "drag_and_drop") {
    std::optional<int> from_index = action.FindInt("from_index");
    if (!from_index.has_value()) {
      std::move(done).Reject(
          "invalid_args",
          "drag_and_drop: missing required `from_index`");
      return;
    }
    std::optional<int> to_index = action.FindInt("to_index");
    if (!to_index.has_value()) {
      std::move(done).Reject("invalid_args",
                             "drag_and_drop: missing required `to_index`");
      return;
    }
    RunDragAndDropAction(*from_index, *to_index,
                         OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }

  if (action_type == "navigate") {
    std::optional<std::string> url =
        RequiredStringArg(action, action_type, "url", done);
    if (!url.has_value()) {
      return;
    }
    RunNavigateAction(OptionalIntArg(action, "tab_id"), std::move(*url),
                      std::move(done));
    return;
  }

  if (action_type == "scroll_to") {
    std::optional<int> index = RequiredElementIndex(action, action_type, done);
    if (!index.has_value()) {
      return;
    }
    RunScrollToAction(*index, OptionalIntArg(action, "tab_id"),
                      std::move(done));
    return;
  }

  if (action_type == "scroll") {
    std::optional<std::string> direction =
        RequiredStringArg(action, action_type, "direction", done);
    if (!direction.has_value()) {
      return;
    }
    std::optional<double> distance = OptionalNumberArg(action, "distance");
    if (!distance.has_value()) {
      std::move(done).Reject(
          "invalid_args",
          "scroll: missing required `distance` (pixels; e.g. 400 for one "
          "viewport-page; values <=10 are interpreted as wheel-notches and "
          "expanded ~120x)");
      return;
    }
    double distance_value = *distance;
    if (distance_value > 0 && distance_value <= 10) {
      // Heuristic: small values like `3` are almost certainly "lines"
      // or wheel notches (the model misread the schema). The actor
      // proto wants pixels and tends to no-op or reject sub-pixel
      // scrolls. One wheel notch ≈ 120px, so scale into a sensible
      // pixel count rather than failing opaquely.
      distance_value *= 120;
    }
    if (distance_value <= 0) {
      std::move(done).Reject(
          "invalid_args",
          base::StrCat({"scroll `distance` must be > 0 pixels; got: ",
                        base::NumberToString(*distance)}));
      return;
    }
    RunScrollAction(OptionalElementIndex(action),
                    OptionalIntArg(action, "tab_id"), std::move(*direction),
                    distance_value, std::move(done));
    return;
  }

  if (action_type == "wait") {
    RunWaitAction(OptionalIntArg(action, "tab_id"),
                  OptionalIntArg(action, "wait_ms"), std::move(done));
    return;
  }
  if (action_type == "back") {
    RunBackAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }
  if (action_type == "forward") {
    RunForwardAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }

  if (action_type == "search") {
    std::optional<std::string> query =
        RequiredStringArg(action, action_type, "query", done);
    if (!query.has_value()) {
      return;
    }
    RunSearchAction(std::move(*query), OptionalStringArg(action, "engine"),
                    std::move(done));
    return;
  }

  if (action_type == "create_tab") {
    std::optional<std::string> url =
        RequiredStringArg(action, action_type, "url", done);
    if (!url.has_value()) {
      return;
    }
    RunCreateTabAction(std::move(*url),
                       OptionalBoolArg(action, "reuse_visible_tab"),
                       std::move(done));
    return;
  }

  if (action_type == "close_tab") {
    RunCloseTabAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }
  if (action_type == "activate_tab") {
    std::optional<int> tab_id =
        RequiredIntArg(action, action_type, "tab_id", done);
    if (!tab_id.has_value()) {
      return;
    }
    RunActivateTabAction(*tab_id, std::move(done));
    return;
  }

  if (action_type == "media_play") {
    RunMediaPlayAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }
  if (action_type == "media_pause") {
    RunMediaPauseAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }
  if (action_type == "media_seek") {
    std::optional<int> seek_time_ms =
        RequiredIntArg(action, action_type, "seek_time_ms", done);
    if (!seek_time_ms.has_value()) {
      return;
    }
    RunMediaSeekAction(OptionalIntArg(action, "tab_id"), *seek_time_ms,
                       std::move(done));
    return;
  }
  if (action_type == "media_state") {
    RunMediaStateAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }

  if (action_type == "screenshot") {
    RunScreenshotAction(OptionalIntArg(action, "tab_id"), std::move(done));
    return;
  }
  if (action_type == "read_page") {
    RunReadPageAction(OptionalIntArg(action, "tab_id"),
                      OptionalStringArg(action, "scope"),
                      OptionalIntArg(action, "max_chars"), std::move(done));
    return;
  }
  if (action_type == "read_element") {
    std::optional<int> index = RequiredElementIndex(action, action_type, done);
    if (!index.has_value()) {
      return;
    }
    RunReadElementAction(*index, OptionalIntArg(action, "tab_id"),
                         OptionalIntArg(action, "max_chars"), std::move(done));
    return;
  }
  if (action_type == "find_text") {
    std::optional<std::string> query =
        RequiredStringArg(action, action_type, "query", done);
    if (!query.has_value()) {
      return;
    }
    RunFindTextAction(OptionalIntArg(action, "tab_id"), std::move(*query),
                      OptionalIntArg(action, "max_results"), std::move(done));
    return;
  }

  std::move(done).Reject(
      "invalid_args",
      base::StrCat({"browser_actions: unsupported action type `",
                    action_type, "`"}));
}

void BrowserTool::RunNavigateAction(std::optional<int> tab_id,
                                    std::string url,
                                    ToolCallback done) {
  // No `url.empty()` guard — `ToolArgExtractor<std::string>` rejects
  // an empty required string with `invalid_args` before we get here.

  // The actor's CreateNavigateRequest rejects if `tab_id` is unset
  // (`tab_handle == TabHandle::Null()`), so we must always emit one —
  // fall back to the active tab when the LLM didn't supply one.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "navigate", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  MaybePrependActivateTab(&actions, *resolved_tab_id, GetProfile());
  apc::NavigateAction* navigate = actions.add_actions()->mutable_navigate();
  navigate->mutable_url()->assign(url);
  navigate->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "navigate", std::move(done));
}

void BrowserTool::RunBackAction(std::optional<int> tab_id, ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "back", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  MaybePrependActivateTab(&actions, *resolved_tab_id, GetProfile());
  apc::HistoryBackAction* back = actions.add_actions()->mutable_back();
  back->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "back", std::move(done));
}

void BrowserTool::RunForwardAction(std::optional<int> tab_id,
                                   ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "forward", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  MaybePrependActivateTab(&actions, *resolved_tab_id, GetProfile());
  apc::HistoryForwardAction* forward =
      actions.add_actions()->mutable_forward();
  forward->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "forward", std::move(done));
}

void BrowserTool::RunSearchAction(std::string query,
                                  std::optional<std::string> engine,
                                  ToolCallback done) {
  // No `query.empty()` guard — the framework's string extractor rejects
  // empty required strings with `invalid_args` before this body runs.

  // Engine → results-page URL template. Each template uses `q` as the
  // query placeholder so `base::ReplaceFirstSubstringAfterOffset` keeps
  // the wiring uniform. The set is intentionally small — the LLM can
  // always fall back to `navigate` for arbitrary URLs.
  std::string engine_id = engine.value_or("google");
  // Lowercase ASCII fold so "Baidu" / "BAIDU" / "baidu" all match.
  std::string engine_lower;
  engine_lower.reserve(engine_id.size());
  for (char c : engine_id) {
    engine_lower.push_back((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
  }
  std::string url;
  const std::string encoded =
      base::EscapeQueryParamValue(query, /*use_plus=*/true);
  if (engine_lower == "baidu") {
    url = base::StrCat({"https://www.baidu.com/s?wd=", encoded});
  } else if (engine_lower == "bing") {
    url = base::StrCat({"https://www.bing.com/search?q=", encoded});
  } else if (engine_lower == "duckduckgo" || engine_lower == "ddg") {
    url = base::StrCat({"https://duckduckgo.com/?q=", encoded});
  } else {
    // google (default) and any unrecognized engine fall through here so
    // a typo in the engine name still lands the user on a results page
    // instead of failing the call.
    url = base::StrCat({"https://www.google.com/search?q=", encoded});
  }
  // Reuse=false is the right default for "the user asked for a search" —
  // it keeps the active tab in flow. The LLM can pass `false` when it
  // wants to preserve the previous page (e.g. compare two queries).
  RunCreateTabAction(std::move(url),
                     std::optional<bool>(false),
                     std::move(done));
}

void BrowserTool::RunCreateTabAction(std::string url,
                                     std::optional<bool> reuse_visible_tab,
                                     ToolCallback done) {
  // No `url.empty()` guard — empty required strings are rejected by
  // the framework's `ToolArgExtractor<std::string>`.
  Profile* profile = GetProfile();

  // ---- Reuse-visible-tab path ------------------------------------------
  // When the LLM passes `reuse_visible_tab=true` it just wants to navigate
  // the active tab. One PerformActions, no chained CreateTab.
  if (reuse_visible_tab.value_or(false)) {
    std::optional<int32_t> active_tab = GetActiveTabHandleId(profile);
    if (!active_tab.has_value()) {
      std::move(done).Reject(
          "no_active_tab",
          "create_tab(reuse_visible_tab=true): no active tab");
      return;
    }
    apc::Actions actions;
    apc::NavigateAction* nav = actions.add_actions()->mutable_navigate();
    nav->mutable_url()->assign(url);
    nav->set_tab_id(*active_tab);
    RunActorAction(std::move(actions), "create_tab", std::move(done));
    return;
  }

  // ---- New-tab path ----------------------------------------------------
  // CreateTabAction.window_id is required by `CreateCreateTabRequest`
  // (`!action.has_window_id()` returns nullptr → "failed to build
  // ToolRequest"), so resolve the active window's SessionID up front.
  std::optional<int32_t> active_window = GetActiveWindowSessionId(profile);
  if (!active_window.has_value()) {
    std::move(done).Reject(
        "no_active_window",
        "create_tab: no active window available to create a tab in");
    return;
  }

  // Materialize the actor task and snapshot its known-tabs set BEFORE
  // the create. After the CreateTab settles, the new tab will have been
  // added to this set via `TabManagementTool::UpdateTaskAfterInvoke` →
  // `ActorTask::AddTab`, so a set diff identifies the new TabHandle.
  std::optional<actor::TaskId> task_id = EnsureTaskCreated(done);
  if (!task_id.has_value()) {
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject(
        "no_actor_service",
        "create_tab: ActorKeyedService unavailable");
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id);
  if (!task) {
    std::move(done).Reject(
        "task_missing",
        "create_tab: actor task no longer exists");
    return;
  }
  actor::ActorTask::TabHandleSet tabs_before = task->GetTabs();

  // Build and dispatch the standalone CreateTab action.
  apc::Actions create_actions;
  create_actions.set_task_id(task_id->value());
  apc::CreateTabAction* create =
      create_actions.add_actions()->mutable_create_tab();
  create->set_window_id(*active_window);
  create->set_foreground(true);

  actor::BuildToolRequestResult tool_request_result =
      actor::BuildToolRequest(create_actions);
  if (!tool_request_result.has_value()) {
    std::move(done).Reject(
        "invalid_action",
        "create_tab: failed to build CreateTab ToolRequest");
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  service->PerformActions(
      *task_id, std::move(tool_request_result.value()),
      actor::ActorTaskMetadata{},
      base::BindOnce(&BrowserTool::OnCreateTabActionDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::move(url), std::move(tabs_before), start_time));
}

void BrowserTool::OnCreateTabActionDone(
    ToolCallback done,
    std::string url,
    actor::ActorTask::TabHandleSet tabs_before,
    base::TimeTicks /*start_time*/,
    actor::mojom::ActionResultCode result_code,
    std::optional<size_t> /*index_of_failed_action*/,
    std::vector<actor::ActionResultWithLatencyInfo> /*action_results*/) {
  if (!actor::IsOk(result_code)) {
    std::move(done).Reject(
        "create_tab_failed",
        base::StrCat({"create_tab: actor CreateTab failed with code ",
                      base::NumberToString(static_cast<int>(result_code))}));
    return;
  }
  Profile* profile = GetProfile();
  if (!profile || !task_id_.has_value()) {
    std::move(done).Reject(
        "task_missing",
        "create_tab: task gone before navigate phase");
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject(
        "no_actor_service",
        "create_tab: ActorKeyedService disappeared before navigate");
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_);
  if (!task) {
    std::move(done).Reject(
        "task_missing",
        "create_tab: actor task gone before navigate phase");
    return;
  }

  // Locate the freshly-created tab by diffing the task's tab set
  // against the pre-create snapshot. `TabManagementTool` calls
  // `task.AddTab(target_tab_)` in `UpdateTaskAfterInvoke`, so by the
  // time this callback fires the new TabHandle is in `task->GetTabs()`.
  std::optional<tabs::TabHandle> new_tab;
  for (const tabs::TabHandle& handle : task->GetTabs()) {
    if (tabs_before.find(handle) == tabs_before.end()) {
      new_tab = handle;
      break;
    }
  }
  if (!new_tab.has_value()) {
    std::move(done).Reject(
        "tab_not_observed",
        "create_tab: created tab not visible in task tab set");
    return;
  }

  // Stage 2 — navigate the new tab. Reuse `RunActorAction` so the
  // standard observation pipeline (BuildActionsResultWithObservations
  // → element-registry refresh → SimpleObservation JSON) runs and the
  // LLM gets a fresh, navigated-page observation back.
  apc::Actions nav_actions;
  apc::NavigateAction* nav = nav_actions.add_actions()->mutable_navigate();
  nav->mutable_url()->assign(url);
  nav->set_tab_id(new_tab->raw_value());
  RunActorAction(std::move(nav_actions), "create_tab", std::move(done));
}

void BrowserTool::RunCloseTabAction(std::optional<int> tab_id,
                                    ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "close_tab", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::CloseTabAction* close_tab =
      actions.add_actions()->mutable_close_tab();
  close_tab->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "close_tab", std::move(done));
}

void BrowserTool::RunActivateTabAction(int tab_id, ToolCallback done) {
  // `tab_id` is declared `NEXUS_INPUT(Integer, ...)` (required) on the
  // schema, so the framework's missing-arg check rejects with
  // `invalid_args` before this body runs — no manual presence guard
  // needed.
  apc::Actions actions;
  apc::ActivateTabAction* activate_tab =
      actions.add_actions()->mutable_activate_tab();
  activate_tab->set_tab_id(tab_id);
  RunActorAction(std::move(actions), "activate_tab", std::move(done));
}

// ----------------------------------------------------------------------
// MediaSession controls.
//
// `MediaControlAction` is a oneof of {play, pause, seek}; calling
// `mutable_play()` / `mutable_pause()` / `mutable_seek()` selects
// the variant. The play / pause messages are empty — instantiating
// the sub-message is enough to set the discriminator. `seek` carries
// `seek_time_milliseconds` (int64); `seek_time_microseconds` is the
// proto's deprecated path and we deliberately don't populate it.
// ----------------------------------------------------------------------

void BrowserTool::RunMediaPlayAction(std::optional<int> tab_id,
                                     ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "media_play", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  MaybePrependActivateTab(&actions, *resolved_tab_id, GetProfile());
  apc::MediaControlAction* media =
      actions.add_actions()->mutable_media_control();
  media->mutable_play();
  media->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "media_play", std::move(done));
}

void BrowserTool::RunMediaPauseAction(std::optional<int> tab_id,
                                      ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "media_pause", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  MaybePrependActivateTab(&actions, *resolved_tab_id, GetProfile());
  apc::MediaControlAction* media =
      actions.add_actions()->mutable_media_control();
  media->mutable_pause();
  media->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "media_pause", std::move(done));
}

void BrowserTool::RunMediaSeekAction(std::optional<int> tab_id,
                                     int seek_time_ms,
                                     ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "media_seek", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  MaybePrependActivateTab(&actions, *resolved_tab_id, GetProfile());
  apc::MediaControlAction* media =
      actions.add_actions()->mutable_media_control();
  // Negative seek positions are clamped to 0 — the proto doc says
  // "should be non-negative" and the renderer rejects negatives, so
  // catching it here gives a cleaner result than a generic actor
  // error.
  media->mutable_seek()->set_seek_time_milliseconds(
      static_cast<int64_t>(seek_time_ms < 0 ? 0 : seek_time_ms));
  media->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "media_seek", std::move(done));
}

void BrowserTool::RunMediaStateAction(std::optional<int> tab_id,
                                      ToolCallback done) {
  // Synchronous read: no actor task, no PerformActions round-trip,
  // no APC fetch. We just resolve the target WebContents and read
  // MediaSession's three sync accessors.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "media_state", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(*resolved_tab_id).Get();
  if (!tab || !tab->GetContents()) {
    std::move(done).Reject(
        "tab_not_found",
        base::StrCat({"media_state: tab_id ",
                      base::NumberToString(*resolved_tab_id),
                      " is no longer alive"}));
    return;
  }

  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "media_state");
  result.Set("tab_id", *resolved_tab_id);

  // `GetIfExists` returns null when no media has ever been associated
  // with the tab — common before the user presses play. We surface
  // this as `has_session:false` (NOT an error) so the LLM can branch
  // without hitting an exception path.
  content::MediaSession* session =
      content::MediaSession::GetIfExists(tab->GetContents());
  if (!session) {
    result.Set("has_session", false);
    std::move(done).Resolve(std::move(result));
    return;
  }
  result.Set("has_session", true);

  // Playback state (playing/paused). The mojom enum has only those
  // two values today; we stringify for LLM legibility.
  if (auto info = session->GetMediaSessionInfoSync()) {
    const char* state =
        info->playback_state ==
                media_session::mojom::MediaPlaybackState::kPlaying
            ? "playing"
            : "paused";
    result.Set("playback_state", state);
  }

  // Position / duration / rate. Pages that don't call
  // `navigator.mediaSession.setPositionState()` won't populate this —
  // we just omit the fields rather than emit fake zeros, so the LLM
  // can tell "I don't know the duration" from "duration is 0".
  if (auto position = session->GetMediaSessionPosition()) {
    result.Set(
        "position_ms",
        static_cast<int>(
            position->GetPosition().InMilliseconds()));
    result.Set(
        "duration_ms",
        static_cast<int>(position->duration().InMilliseconds()));
    result.Set("playback_rate", position->playback_rate());
    if (position->end_of_media()) {
      result.Set("end_of_media", true);
    }
  }

  // Page-supplied metadata. UTF-16 → UTF-8 because base::Value strings
  // are UTF-8. Empty fields are omitted so the LLM doesn't waste
  // attention on blanks; pages frequently set only a subset.
  const media_session::MediaMetadata& meta =
      session->GetMediaSessionMetadata();
  auto set_if_nonempty = [&](std::string_view key,
                             const std::u16string& value) {
    if (!value.empty()) {
      result.Set(key, base::UTF16ToUTF8(value));
    }
  };
  set_if_nonempty("title", meta.title);
  set_if_nonempty("artist", meta.artist);
  set_if_nonempty("album", meta.album);
  set_if_nonempty("source_title", meta.source_title);

  std::move(done).Resolve(std::move(result));
}

void BrowserTool::RunWaitAction(std::optional<int> tab_id,
                                std::optional<int> wait_ms,
                                ToolCallback done) {
  apc::Actions actions;
  if (tab_id.has_value()) {
    MaybePrependActivateTab(&actions, *tab_id, GetProfile());
  }
  apc::WaitAction* wait = actions.add_actions()->mutable_wait();
  if (wait_ms.has_value()) {
    wait->set_wait_time_ms(*wait_ms);
  }
  if (tab_id.has_value()) {
    wait->set_observe_tab_id(*tab_id);
  }
  RunActorAction(std::move(actions), "wait", std::move(done));
}

// ----------------------------------------------------------------------
// Index-based actions. The LLM-facing `index` is the APC
// `content_node_id` directly, looked up via NodeRegistry to recover
// the (document_id, tab_id) needed for the actor proto. With explicit
// `tab_id`, lookup is unambiguous; without it, the registry returns
// the most-recently-registered tab for that index.
// ----------------------------------------------------------------------

void BrowserTool::RunClickAction(int node_id,
                                 std::optional<int> tab_id,
                                 std::optional<std::string> button,
                                 std::optional<std::string> click_count,
                                 ToolCallback done) {
  const NodeRegistry::Entry* entry =
      LookupNode(node_id, tab_id, done, "click");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  MaybePrependActivateTab(&actions, entry->tab_id, GetProfile());
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);

  click->set_click_type(button.has_value() && *button == "right"
                            ? apc::ClickAction::RIGHT
                            : apc::ClickAction::LEFT);
  click->set_click_count(click_count.has_value() && *click_count == "double"
                             ? apc::ClickAction::DOUBLE
                             : apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "click", std::move(done));
}

void BrowserTool::RunInputAction(int node_id,
                                 std::optional<int> tab_id,
                                 std::string text,
                                 std::optional<bool> submit,
                                 ToolCallback done) {
  const NodeRegistry::Entry* entry =
      LookupNode(node_id, tab_id, done, "input");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  MaybePrependActivateTab(&actions, entry->tab_id, GetProfile());
  apc::TypeAction* type = actions.add_actions()->mutable_type();
  type->mutable_target()->set_content_node_id(node_id);
  type->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  type->set_tab_id(entry->tab_id);
  type->set_text(std::move(text));
  // The actor's TypeAction currently only supports DELETE_EXISTING (the
  // text replaces existing content). The JS spec also documents this
  // as the contract for input.
  type->set_mode(apc::TypeAction::DELETE_EXISTING);
  type->set_follow_by_enter(submit.value_or(false));
  RunActorAction(std::move(actions), "input", std::move(done));
}

void BrowserTool::RunSelectAction(int node_id,
                                  std::optional<int> tab_id,
                                  std::string value,
                                  ToolCallback done) {
  const NodeRegistry::Entry* entry =
      LookupNode(node_id, tab_id, done, "select");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  // Explicit tab_id wins; the registry's home tab is the fallback.
  const int32_t resolved_tab_id = tab_id.value_or(entry->tab_id);
  MaybePrependActivateTab(&actions, resolved_tab_id, GetProfile());
  apc::SelectAction* select = actions.add_actions()->mutable_select();
  select->mutable_target()->set_content_node_id(node_id);
  select->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  select->set_tab_id(resolved_tab_id);
  select->set_value(std::move(value));
  RunActorAction(std::move(actions), "select", std::move(done));
}

void BrowserTool::RunHoverAction(int node_id,
                                 std::optional<int> tab_id,
                                 ToolCallback done) {
  const NodeRegistry::Entry* entry =
      LookupNode(node_id, tab_id, done, "hover");
  if (!entry) {
    return;
  }
  apc::Actions actions;
  // Explicit tab_id wins; node's home tab is the fallback. Same
  // convention as select / scroll_to.
  const int32_t resolved_tab_id = tab_id.value_or(entry->tab_id);
  MaybePrependActivateTab(&actions, resolved_tab_id, GetProfile());
  apc::MoveMouseAction* move = actions.add_actions()->mutable_move_mouse();
  move->mutable_target()->set_content_node_id(node_id);
  move->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  move->set_tab_id(resolved_tab_id);
  RunActorAction(std::move(actions), "hover", std::move(done));
}

void BrowserTool::RunDragAndDropAction(int from_node_id,
                                       int to_node_id,
                                       std::optional<int> tab_id,
                                       ToolCallback done) {
  // LookupNode only consumes `done` on miss, so the second call is
  // safe whenever the first succeeded. Both rejections (miss + same-tab
  // mismatch) feed back through the same `done`.
  const NodeRegistry::Entry* from_entry =
      LookupNode(from_node_id, tab_id, done, "drag_and_drop");
  if (!from_entry) {
    return;
  }
  const NodeRegistry::Entry* to_entry =
      LookupNode(to_node_id, tab_id, done, "drag_and_drop");
  if (!to_entry) {
    return;
  }
  if (from_entry->tab_id != to_entry->tab_id) {
    // The actor's DragAndReleaseAction takes a single tab_id and
    // operates within one renderer; allowing different home tabs for
    // the two endpoints would silently drop one of them. Reject up
    // front with a clear message instead.
    std::move(done).Reject(
        "invalid_args",
        base::StrCat({"drag_and_drop: from_index ",
                      base::NumberToString(from_node_id), " (tab=",
                      base::NumberToString(from_entry->tab_id),
                      ") and to_index ",
                      base::NumberToString(to_node_id), " (tab=",
                      base::NumberToString(to_entry->tab_id),
                      ") are on different tabs"}));
    return;
  }
  apc::Actions actions;
  const int32_t resolved_tab_id = tab_id.value_or(from_entry->tab_id);
  MaybePrependActivateTab(&actions, resolved_tab_id, GetProfile());
  apc::DragAndReleaseAction* drag =
      actions.add_actions()->mutable_drag_and_release();
  drag->mutable_from_target()->set_content_node_id(from_node_id);
  drag->mutable_from_target()
      ->mutable_document_identifier()
      ->set_serialized_token(from_entry->document_id);
  drag->mutable_to_target()->set_content_node_id(to_node_id);
  drag->mutable_to_target()
      ->mutable_document_identifier()
      ->set_serialized_token(to_entry->document_id);
  drag->set_tab_id(resolved_tab_id);
  RunActorAction(std::move(actions), "drag_and_drop", std::move(done));
}

void BrowserTool::RunScrollToAction(int node_id,
                                    std::optional<int> tab_id,
                                    ToolCallback done) {
  const NodeRegistry::Entry* entry =
      LookupNode(node_id, tab_id, done, "scroll_to");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  const int32_t resolved_tab_id = tab_id.value_or(entry->tab_id);
  MaybePrependActivateTab(&actions, resolved_tab_id, GetProfile());
  apc::ScrollToAction* scroll_to =
      actions.add_actions()->mutable_scroll_to();
  scroll_to->mutable_target()->set_content_node_id(node_id);
  scroll_to->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  scroll_to->set_tab_id(resolved_tab_id);
  RunActorAction(std::move(actions), "scroll_to", std::move(done));
}

void BrowserTool::RunScrollAction(std::optional<int> node_id,
                                  std::optional<int> tab_id,
                                  std::string direction,
                                  double distance,
                                  ToolCallback done) {
  // Resolve the target tab first so we can prepend ActivateTab before
  // building the scroll action proto.
  int32_t resolved_tab_id = 0;
  const NodeRegistry::Entry* entry = nullptr;
  if (node_id.has_value()) {
    entry = LookupNode(*node_id, tab_id, done, "scroll");
    if (!entry) {
      return;
    }
    resolved_tab_id = tab_id.value_or(entry->tab_id);
  } else {
    // Viewport scroll — the actor's `BuildToolRequest` requires a
    // tab_id on every ScrollAction (even with no node target), so
    // resolve to the active tab when the LLM omits tab_id. Without
    // this fallback the system-prompted shape
    // `{type:"scroll", direction:"down", distance:N}` (no index, no
    // tab_id) gets opaquely rejected with `invalid_action`, sending
    // the agent into a wasted-turn loop chasing what looks like a
    // schema mismatch.
    std::optional<int32_t> resolved =
        ResolveTabIdOrReject(tab_id, GetProfile(), "scroll", done);
    if (!resolved.has_value()) {
      return;
    }
    resolved_tab_id = *resolved;
  }

  apc::Actions actions;
  MaybePrependActivateTab(&actions, resolved_tab_id, GetProfile());
  apc::ScrollAction* scroll = actions.add_actions()->mutable_scroll();
  if (entry != nullptr) {
    scroll->mutable_target()->set_content_node_id(*node_id);
    scroll->mutable_target()
        ->mutable_document_identifier()
        ->set_serialized_token(entry->document_id);
  }
  scroll->set_tab_id(resolved_tab_id);

  // Map the LLM-facing direction string to the actor's enum. Reject
  // values that aren't in the NEXUS_INPUT enum list.
  if (direction == "up") {
    scroll->set_direction(apc::ScrollAction::UP);
  } else if (direction == "down") {
    scroll->set_direction(apc::ScrollAction::DOWN);
  } else if (direction == "left") {
    scroll->set_direction(apc::ScrollAction::LEFT);
  } else if (direction == "right") {
    scroll->set_direction(apc::ScrollAction::RIGHT);
  } else {
    std::move(done).Reject(
        "invalid_args",
        base::StrCat({"scroll `direction` must be one of "
                      "up/down/left/right; got: \"", direction, "\""}));
    return;
  }

  scroll->set_distance(static_cast<float>(distance));
  RunActorAction(std::move(actions), "scroll", std::move(done));
}

// ----------------------------------------------------------------------
// Observation tools: bypass the actor `PerformActions` path entirely.
// We mark the target tab as "observe-once" on the actor task, then call
// `BuildActionsResultWithObservations` directly with no actions — the
// framework still fetches APC + screenshot for the marked tabs because
// it walks `task->GetLastActedTabs()`, into which `ObserveTabOnce`
// inserted the tab.
// ----------------------------------------------------------------------

void BrowserTool::RunTabObservation(int32_t tab_id_value,
                                     std::string_view action_name,
                                     ToolCallback done,
                                     ObservationCompletion completion) {
  // Same per-action budget the actor-driven path uses — observation-
  // only actions (screenshot, read_page, find_text) also dispatch
  // through the actor framework
  // and can hang on a stalled APC fetch.
  done = PausableBudgetWrap(std::move(done), action_timeout_,
                            std::string(action_name));

  Profile* profile = GetProfile();
  if (!profile) {
    std::move(done).Reject(
        "no_profile",
        base::StrCat({action_name, ": no profile"}));
    return;
  }
  std::optional<actor::TaskId> task_id_opt = EnsureTaskCreated(done);
  if (!task_id_opt.has_value()) {
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject(
        "no_actor_service",
        base::StrCat({action_name, ": ActorKeyedService unavailable"}));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_opt);
  if (!task) {
    std::move(done).Reject(
        "task_missing",
        base::StrCat({action_name, ": actor task no longer exists"}));
    return;
  }

  // Validate the target tab is live before handing it to the actor task.
  // ObserveTabOnce silently no-ops on a dead handle, but a silent no-op
  // would leave the upstream pipeline walking only `controlled_tabs_`
  // (potentially a stale carryover from a prior action whose
  // ActorTabData::last_observed_page_content_ is unsafe to overwrite),
  // which is the crash window described at the
  // OnActorPerformActionsDone guard above.
  tabs::TabHandle target_handle(tab_id_value);
  if (!target_handle.Get()) {
    std::move(done).Reject(
        "tab_gone",
        base::StrCat({action_name, ": target tab no longer exists"}));
    return;
  }
  task->ObserveTabOnce(target_handle);

  // BuildActionsResultWithObservations walks `task->GetLastActedTabs()`
  // (= controlled_tabs_ ∪ to_observe_tabs_), not `GetTabs()` — so the
  // empty-set guard mirrored from OnActorPerformActionsDone has to be
  // checked against that union here.
  if (task->GetLastActedTabs().empty()) {
    std::move(done).Reject(
        "no_tabs",
        base::StrCat({action_name,
                      ": actor task has no live tabs for observation"}));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  actor::BuildActionsResultWithObservations(
      *profile, start_time,
      actor::mojom::ActionResultCode::kOk,
      /*index_of_failed_action=*/std::nullopt,
      /*action_results=*/{},
      *task,
      /*skip_async_observation_information=*/false,
      base::BindOnce(std::move(completion), std::move(done),
                     std::string(action_name)));
}

void BrowserTool::RunScreenshotAction(std::optional<int> tab_id,
                                      ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "screenshot", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // The standard `OnActionsResultBuilt` already produces a SimpleObservation
  // that includes `has_screenshot: true` whenever the framework attached
  // a non-empty screenshot blob to the TabObservation, so reusing it is
  // exactly what we want here.
  RunTabObservation(
      *resolved_tab_id, "screenshot", std::move(done),
      base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                     weak_ptr_factory_.GetWeakPtr()));
}

namespace {

// Trims `s` so it ends on a UTF-8 character boundary. If a multi-byte
// UTF-8 sequence is partially present at the tail (because we sliced
// at a raw byte index that fell inside it), drops the partial bytes
// so the result is well-formed.
//
// Why this exists: `base::Value`'s string ctors / setters CHECK that
// the input is valid UTF-8 (see values.cc's `IsStringUTF8AllowingNoncharacters`
// path). On a Chinese-language page each character is encoded as 3
// UTF-8 bytes; a byte-level `resize(N)` or `append(s, 0, N)` very
// often falls inside one of those sequences and produces invalid
// UTF-8, which crashes the renderer/process via DCHECK on the next
// `result.Set("page_text", page_text)` call. Walking back to the most
// recent leading byte and dropping any incomplete trailing sequence
// is enough to satisfy the validator.
void EnsureUtf8TailComplete(std::string& s) {
  if (s.empty()) {
    return;
  }
  // Walk back from the last byte until we find a leading byte (one
  // whose top two bits are NOT `10`). Continuation bytes match
  // `(b & 0xC0) == 0x80`.
  size_t i = s.size();
  while (i > 0) {
    --i;
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if ((b & 0xC0) == 0x80) {
      continue;  // continuation byte
    }
    // Determine the expected sequence length from the leading byte.
    int len = 1;
    if ((b & 0x80) == 0x00) {
      len = 1;  // ASCII
    } else if ((b & 0xE0) == 0xC0) {
      len = 2;
    } else if ((b & 0xF0) == 0xE0) {
      len = 3;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
    }
    const size_t tail_len = s.size() - i;
    if (tail_len == static_cast<size_t>(len)) {
      // Complete sequence — keep as-is.
      return;
    }
    // Sequence at `i` is incomplete. Drop it (and everything after).
    s.resize(i);
    return;
  }
  // No leading byte found at all (corrupt input) — clear it.
  s.clear();
}

// Joins the leaf text of every TEXT / PARAGRAPH / HEADING / LIST_ITEM
// node beneath `node` into `out`, separating consecutive text fragments
// with a single newline so the LLM can reason about paragraph breaks.
// Stops appending once `out` reaches `max_chars` and returns `false`
// to signal the walker to abort early.
bool AppendVisibleText(const apc::ContentNode& node,
                       int max_chars,
                       std::string& out) {
  if (static_cast<int>(out.size()) >= max_chars) {
    return false;
  }
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    apc::ContentAttributeType t = attrs.attribute_type();
    const bool is_text_carrier =
        t == apc::CONTENT_ATTRIBUTE_TEXT ||
        t == apc::CONTENT_ATTRIBUTE_PARAGRAPH ||
        t == apc::CONTENT_ATTRIBUTE_HEADING ||
        t == apc::CONTENT_ATTRIBUTE_LIST_ITEM;
    if (is_text_carrier && attrs.has_text_data()) {
      const std::string& s = attrs.text_data().text_content();
      if (!s.empty()) {
        if (!out.empty()) {
          out.push_back('\n');
        }
        const int budget = max_chars - static_cast<int>(out.size());
        if (budget <= 0) {
          return false;
        }
        if (static_cast<int>(s.size()) <= budget) {
          out.append(s);
        } else {
          // Byte-level slice may fall in the middle of a multi-byte
          // UTF-8 sequence (Chinese / Japanese / Korean / emoji etc.).
          // Append the prefix then walk back to the last full sequence
          // boundary so `out` stays valid UTF-8 — base::Value::Set
          // CHECKs UTF-8 validity and crashes the process otherwise.
          out.append(s, 0, static_cast<size_t>(budget));
          EnsureUtf8TailComplete(out);
          return false;
        }
      }
    }
  }
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (!AppendVisibleText(child, max_chars, out)) {
      return false;
    }
  }
  return true;
}

// Lower-cases ASCII characters in-place. The LLM-supplied query is
// expected to be plain ASCII for now; full Unicode case folding would
// require ICU, which we can wire later when a real CJK case-fold use
// case shows up.
std::string ToLowerAscii(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    out.push_back((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
  }
  return out;
}

// Recursive walker for `find_text`. For every node whose visible
// text contains `query_lower`, appends a `matches[]` entry with the text
// plus its nearest actionable ancestor's index / type / text (so
// the LLM can hand the ancestor's index straight to `click`).
//
// The APC content_node_id is the LLM-facing index, so we emit
// `attrs.common_ancestor_dom_node_id()` directly.
struct FindTextWalker {
  // Owned (not a view) — the walker outlives the temporary returned by
  // ToLowerAscii(query) at construction time, so we copy the lowered
  // form into the struct.
  std::string query_lower;
  int max_results = 5;
  base::ListValue matches;
  // Stack of nearest actionable ancestor indexes.
  std::vector<int> ancestor_node_id_stack;
  std::vector<std::string> ancestor_type_stack;
  std::vector<std::string> ancestor_text_stack;
};

void FindTextWalk(const apc::ContentNode& node, FindTextWalker& w) {
  if (static_cast<int>(w.matches.size()) >= w.max_results) {
    return;
  }
  bool pushed_ancestor = false;
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    if (IsActionable(attrs) && attrs.has_common_ancestor_dom_node_id()) {
      w.ancestor_node_id_stack.push_back(
          attrs.common_ancestor_dom_node_id());
      w.ancestor_type_stack.push_back(
          ContentAttributeTypeName(attrs.attribute_type()));
      w.ancestor_text_stack.push_back(ExtractNodeText(attrs));
      pushed_ancestor = true;
    }

    // Match check on the current node's leaf text. We use
    // CONTENT_ATTRIBUTE_TEXT / PARAGRAPH / HEADING / LIST_ITEM /
    // ANCHOR / FORM_CONTROL because those are the carriers the LLM
    // would search for.
    if (attrs.has_text_data()) {
      const std::string& raw = attrs.text_data().text_content();
      if (!raw.empty()) {
        const std::string lowered = ToLowerAscii(raw);
        if (lowered.find(w.query_lower) != std::string::npos) {
          base::DictValue match;
          match.Set("text", raw);
          if (!w.ancestor_node_id_stack.empty()) {
            match.Set("clickable_ancestor_index",
                      w.ancestor_node_id_stack.back());
            match.Set("ancestor_type", w.ancestor_type_stack.back());
            if (!w.ancestor_text_stack.back().empty()) {
              match.Set("ancestor_text", w.ancestor_text_stack.back());
            }
          }
          w.matches.Append(std::move(match));
        }
      }
    }
  }
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (static_cast<int>(w.matches.size()) >= w.max_results) {
      break;
    }
    FindTextWalk(child, w);
  }
  if (pushed_ancestor) {
    w.ancestor_node_id_stack.pop_back();
    w.ancestor_type_stack.pop_back();
    w.ancestor_text_stack.pop_back();
  }
}

}  // namespace

void BrowserTool::RunReadPageAction(std::optional<int> tab_id,
                                    std::optional<std::string> scope,
                                    std::optional<int> max_chars,
                                    ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "read_page", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // The actor framework's APC fetch always returns the full page tree
  // — it doesn't expose a viewport-only mode through this entry point.
  // We accept `scope` for forward compatibility and pass it through to
  // the completion path, which currently treats viewport and full_page
  // identically (both walk the entire APC tree). When per-scope APC
  // becomes available, only `OnReadPageObservationBuilt` will need to
  // change.
  std::string scope_value = scope.value_or("viewport");
  int max_chars_value = max_chars.value_or(4000);
  if (max_chars_value <= 0) {
    max_chars_value = 4000;
  }
  RunTabObservation(
      *resolved_tab_id, "read_page", std::move(done),
      base::BindOnce(&BrowserTool::OnReadPageObservationBuilt,
                     weak_ptr_factory_.GetWeakPtr(), std::move(scope_value),
                     max_chars_value, *resolved_tab_id));
}

void BrowserTool::OnReadPageObservationBuilt(
    std::string scope,
    int max_chars,
    int32_t requested_tab_id,
    ToolCallback done,
    std::string action_name,
    base::TimeTicks /*start_time*/,
    actor::mojom::ActionResultCode result_code,
    std::optional<size_t> /*index_of_failed_action*/,
    std::vector<actor::ActionResultWithLatencyInfo> /*action_results*/,
    actor::TaskId /*task_id*/,
    bool /*skip_async_observation_information*/,
    std::unique_ptr<apc::ActionsResult> result_proto,
    std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> /*journal*/) {
  // Node registry accumulates across observations — see comment in
  // OnActionsResultBuilt. BuildObservationDict below registers the
  // new observation's nodes; prior nodes that didn't reappear remain
  // in the registry and surface a clean stale error if used.

  base::DictValue result;
  result.Set("action", action_name);
  result.Set("scope", scope);
  if (!actor::IsOk(result_code)) {
    result.Set("ok", false);
    result.Set("result", DescribeActorError(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }
  if (!result_proto) {
    result.Set("ok", false);
    result.Set("result", "no_observation");
    std::move(done).Resolve(std::move(result));
    return;
  }
  // Refresh the standard SimpleObservation alongside the page_text so
  // the LLM can still pick indexes out of `tabs[].elements[]` if
  // it wants to act after reading.
  base::DictValue observation = BuildObservationDict(*result_proto);
  if (!observation.empty()) {
    result.Set("observation", std::move(observation));
  }

  // Find the requested tab within the observation. The actor framework
  // can return multiple `TabObservation`s in `result_proto->tabs()`
  // (e.g. when it batches observations across recently-acted tabs),
  // and historically we picked the first one — which silently returned
  // the wrong tab's content when navigation had moved focus elsewhere.
  // Match against `requested_tab_id` first; fall back to the first tab
  // with content if the requested tab somehow isn't in the observation.
  const apc::TabObservation* match = nullptr;
  const apc::TabObservation* first_with_content = nullptr;
  for (const apc::TabObservation& tab_obs : result_proto->tabs()) {
    if (!tab_obs.has_annotated_page_content()) {
      continue;
    }
    if (!tab_obs.annotated_page_content().has_root_node()) {
      continue;
    }
    if (!first_with_content) {
      first_with_content = &tab_obs;
    }
    if (tab_obs.id() == requested_tab_id) {
      match = &tab_obs;
      break;
    }
  }

  std::string page_text;
  std::optional<int> picked_tab_id;
  const apc::TabObservation* selected = match ? match : first_with_content;
  if (selected) {
    const apc::AnnotatedPageContent& apc_data =
        selected->annotated_page_content();
    AppendVisibleText(apc_data.root_node(), max_chars, page_text);
    picked_tab_id = selected->id();
    if (apc_data.has_main_frame_data()) {
      if (!apc_data.main_frame_data().url().empty()) {
        result.Set("url", apc_data.main_frame_data().url());
      }
      if (!apc_data.main_frame_data().title().empty()) {
        result.Set("title", apc_data.main_frame_data().title());
      }
    }
    if (!match && first_with_content) {
      // Defensive: requested tab wasn't in the observation set.
      // Surface a warning so the model knows the content came from a
      // different tab and can react (e.g. activate the right tab)
      // instead of silently consuming the wrong page.
      result.Set(
          "tab_mismatch_warning",
          base::StrCat(
              {"requested tab ", base::NumberToString(requested_tab_id),
               " was not in the observation; returning tab ",
               base::NumberToString(picked_tab_id.value_or(-1)),
               " content instead. Consider calling activate_tab first."}));
    }
  }
  if (picked_tab_id.has_value()) {
    result.Set("tab_id", *picked_tab_id);
  }
  result.Set("page_text", page_text);
  result.Set("truncated", static_cast<int>(page_text.size()) >= max_chars);
  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::RunReadElementAction(int node_id,
                                       std::optional<int> tab_id,
                                       std::optional<int> max_chars,
                                       ToolCallback done) {
  // Pure registry lookup. The node_registry already cached the
  // visible label / inner text from the most recent observation, so
  // there's no need to re-fetch the APC tree — the LLM uses this tool
  // for tiny snippets (button labels, list-item text) where the
  // registry's cached `text` is exactly what's wanted.
  const NodeRegistry::Entry* entry =
      LookupNode(node_id, tab_id, done, "read_element");
  if (!entry) {
    return;  // LookupNode already rejected.
  }
  const int max_chars_value =
      max_chars.has_value() && *max_chars > 0 ? *max_chars : 2000;
  std::string text = entry->text;
  bool truncated = false;
  if (static_cast<int>(text.size()) > max_chars_value) {
    text.resize(static_cast<size_t>(max_chars_value));
    // Byte-level resize may bisect a multi-byte UTF-8 sequence on
    // pages with non-ASCII text — see EnsureUtf8TailComplete's docs.
    EnsureUtf8TailComplete(text);
    truncated = true;
  }
  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "read_element");
  result.Set("index", node_id);
  result.Set("type", entry->type);
  result.Set("element_text", std::move(text));
  result.Set("truncated", truncated);
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::RunFindTextAction(std::optional<int> tab_id,
                                    std::string query,
                                    std::optional<int> max_results,
                                    ToolCallback done) {
  // No `query.empty()` guard — the framework's string extractor rejects
  // empty required strings up front.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(tab_id, GetProfile(), "find_text", done);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  const int max_results_value =
      max_results.has_value() && *max_results > 0 ? *max_results : 5;
  RunTabObservation(
      *resolved_tab_id, "find_text", std::move(done),
      base::BindOnce(&BrowserTool::OnFindTextObservationBuilt,
                     weak_ptr_factory_.GetWeakPtr(), std::move(query),
                     max_results_value));
}

void BrowserTool::OnFindTextObservationBuilt(
    std::string query,
    int max_results,
    ToolCallback done,
    std::string action_name,
    base::TimeTicks /*start_time*/,
    actor::mojom::ActionResultCode result_code,
    std::optional<size_t> /*index_of_failed_action*/,
    std::vector<actor::ActionResultWithLatencyInfo> /*action_results*/,
    actor::TaskId /*task_id*/,
    bool /*skip_async_observation_information*/,
    std::unique_ptr<apc::ActionsResult> result_proto,
    std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> /*journal*/) {
  base::DictValue result;
  result.Set("action", action_name);
  result.Set("query", query);
  if (!actor::IsOk(result_code)) {
    result.Set("ok", false);
    result.Set("result", DescribeActorError(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }
  if (!result_proto) {
    result.Set("ok", false);
    result.Set("result", "no_observation");
    std::move(done).Resolve(std::move(result));
    return;
  }

  // Refresh the standard observation (also updates the node registry
  // and last_observation_* fields used by ToolsAdapter for agent_meta).
  // No inverse map needed — index is the LLM-facing id.
  base::DictValue observation = BuildObservationDict(*result_proto);
  if (!observation.empty()) {
    result.Set("observation", std::move(observation));
  }

  // Find walk. Limited to the first APC tree (the active observed
  // tab) since `find_text` is one-tab.
  FindTextWalker walker;
  walker.query_lower = ToLowerAscii(query);
  walker.max_results = max_results;
  for (const apc::TabObservation& tab_obs : result_proto->tabs()) {
    if (!tab_obs.has_annotated_page_content()) {
      continue;
    }
    const apc::AnnotatedPageContent& apc_data = tab_obs.annotated_page_content();
    if (apc_data.has_root_node()) {
      FindTextWalk(apc_data.root_node(), walker);
    }
    break;
  }
  result.Set("matches", std::move(walker.matches));
  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

}  // namespace nexus

// Macro DSL category accessor — descends from `ToolSession` to its
// `browser_tool_` member. Pairs with the `NEXUS_DECLARE_CATEGORY`
// in `browser_tool.h`. Lives at file scope (outside `namespace nexus`)
// because the macro expands to `namespace nexus::detail { ... }` and
// we don't want to nest namespaces.
NEXUS_DEFINE_CATEGORY(nexus::BrowserTool, browser_tool);
