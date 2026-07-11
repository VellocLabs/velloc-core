#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

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
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor_webui.mojom.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"

namespace nexus {

// File-local shorthand: `using` directive scoped to this TU only so we
// can call ResolveTabIdOrReject / MaybePrependActivateTab / etc.
// without qualifying every call site. Bad style in headers; acceptable
// in a single .cc file. Mirrored in browser_tool_actions.cc — both
// files share the same helper namespace and need the same alias.
using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

// All cross-cutting helpers (DescribeActorError, MapStopReason,
// GetActiveBrowserForProfile, GetActiveWindowSessionId,
// GetActiveTabHandleId, ResolveTabIdOrReject, MaybePrependActivateTab,
// GetPolicyChecker, ContentAttributeTypeName, ExtractNodeText,
// IsActionable, TruncateUtf8Snippet) plus the PausableTimeout +
// PausableToolCallbackHolder classes have been moved out to
// browser_tool_internal.{h,cc}. The `using namespace
// browser_tool_internal;` directive at the top of this TU lets us
// continue calling them unqualified from BrowserTool method bodies.

}  // namespace

// PausableTimeout + PausableToolCallbackHolder class definitions live
// in browser_tool_internal.h (alongside the cross-cutting helpers).

// static
void BrowserTool::DefineTools(ToolRegistry& /*registry*/) {
  // Body intentionally empty.
  //
  // Phase 4 retired the macro-based `browser_actions` umbrella tool —
  // every action it dispatched (click, input, navigate, …) is now its
  // own top-level declarative tool whose schema + dispatcher come from
  // generate.py via PopulateRegistry() (called by ToolCatalog right
  // after this no-op). The 24 split tools structurally prevent the
  // empty-args failure mode that motivated the migration: each tool
  // has at least one required field whose absence produces a clean
  // `invalid_args` reject before reaching the body.
  //
  // The orchestration helpers + `BrowserTool::browser_actions(...)`
  // method below are now unreachable through the LLM tool path. They
  // are kept around one more commit so this change is small enough to
  // bisect cleanly; a follow-up commit deletes them along with the
  // anonymous-namespace strict-schema / input-examples helpers above.
  // (See the [[maybe_unused]] annotations on those helpers for
  // pointers to what gets removed next.)
}

BrowserTool::BrowserTool(ToolSession* session, Profile* profile,
                         const std::string& session_key)
    : ToolBase(session, profile, session_key) {}

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

// ContentAttributeTypeName, ExtractNodeText, TruncateUtf8Snippet,
// IsActionable, and `kDescendantTextSnippetMax` live in
// browser_tool_internal.h — this file's `using namespace
// browser_tool_internal;` directive makes them callable here without
// qualification.

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

}  // namespace nexus

// Macro DSL category accessor — descends from `ToolSession` to its
// `browser_tool_` member. Pairs with the `NEXUS_DECLARE_CATEGORY`
// in `browser_tool.h`. Lives at file scope (outside `namespace nexus`)
// because the macro expands to `namespace nexus::detail { ... }` and
// we don't want to nest namespaces.
NEXUS_DEFINE_CATEGORY(nexus::BrowserTool, browser_tool);
