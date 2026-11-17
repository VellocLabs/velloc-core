#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "components/actor/core/task_source_info.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor_webui.mojom.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "url/gurl.h"

namespace nexus {

// File-local shorthand: `using` directive scoped to this TU only so we
// can call ResolveTabIdOrReject / MaybePrependActivateTab / etc.
// without qualifying every call site. Bad style in headers; acceptable
// in a single .cc file. Mirrored in the browser_tool_*.cc action
// files — they share the same helper namespace and need the same alias.
using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

}  // namespace

// PausableTimeout + PausableToolCallbackHolder class definitions live
// in browser_tool_internal.h (alongside the cross-cutting helpers).

BrowserTool::BrowserTool(ToolSession* session, Profile* profile,
                         const std::string& session_key)
    : ToolBase(session, profile, session_key) {}

BrowserTool::~BrowserTool() {
  // 1) Final OnStop pass — idempotent, stops the actor task and clears
  //    the node registry if they survived prior OnStop calls.
  OnStop(OnStopReason::kDestructor);
  // 2) Tab teardown lives in the destructor — fires for kSessionDeleted
  //    (ToolSessionRegistry erases the session), the ~ToolSessionRegistry cascade
  //    (panel close / port-service teardown), and any other path that
  //    destroys the ToolSession. "Tabs close with the panel" is the
  //    chosen UX.
  CloseAllSessionTabsOnDestroy();
}

void BrowserTool::OnStop(OnStopReason reason) {
  LOG(INFO) << "[BrowserTool] OnStop session=" << GetSessionKey()
            << " reason=" << OnStopReasonToString(reason)
            << " has_task=" << task_id_.has_value()
            << " registry_size=" << node_registry_.size();
  if (task_id_.has_value()) {
    auto* service = actor::ActorKeyedService::Get(GetProfile());
    // Diagnostic: if we're being asked to stop a task that's
    // currently kActing or kReflecting, that's almost always a JS-
    // side bug — the agent loop fired `session.onStop` while a tool
    // dispatch was still in flight. Surface this as a noisy WARNING
    // so we can correlate with the JS-side stack trace (see the
    // matching console.error in deps.ts::handleStopHooks).
    if (service) {
      if (actor::ActorTask* task = service->GetTask(*task_id_)) {
        actor::ActorTask::State state = task->GetState();
        if (state == actor::ActorTask::State::kActing ||
            state == actor::ActorTask::State::kReflecting) {
          LOG(WARNING)
              << "[BrowserTool] OnStop(reason="
              << OnStopReasonToString(reason)
              << ") arrived while task " << task_id_->value()
              << " is in active state " << static_cast<int>(state)
              << " (kActing/kReflecting). The in-flight tool's "
                 "PerformActions BindOnce is about to be dropped, "
                 "which will emit tool_dropped to the JS guest. This "
                 "is a JS-side bug — `session.onStop` should not fire "
                 "while a tool is mid-flight. See "
                 "nexus_web/src/query.ts !needsFollowUp branch + "
                 "nexus_web/src/query/deps.ts::handleStopHooks for "
                 "the JS call sites and capture the matching console "
                 "error to identify the trigger. session="
              << GetSessionKey();
        }
      }
      service->StopTask(*task_id_, ToActorStoppedReason(reason));
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
  registry_document_by_tab_.clear();
  in_flight_ = nullptr;
  // A projection staged by an action that was stopped before it resolved
  // never shipped. The committed baseline itself is kept on kCompleted: the
  // model's context is intact and the guest re-marks it stale on the context
  // mutations that matter. On kUserStopped it is NOT kept: a result that
  // committed around the user's Stop is replaced by the guest's interrupted
  // marker, so the model never saw the elements[] the baseline names, and a
  // short-circuit against it would hand the model an {unchanged} stub for a
  // document it has no elements for. One full re-send is the price.
  if (reason == OnStopReason::kUserStopped) {
    observation_baseline_.MarkStale();
  } else {
    observation_baseline_.Discard();
  }
  // Between turns agent tabs follow their real visibility, so a hidden or
  // minimised tab releases its tiles and GPU resources (F-21).
  session_tab_capture_holds_.clear();
  // Note: tabs are NOT closed here. Closure happens in ~BrowserTool
  // so that the OnStop-but-stay-warm reasons (kCompleted /
  // kUserStopped) preserve the user's tabs across turns.
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
             std::string /*message*/, base::DictValue params) {
            h->Reject(std::move(c), std::move(params));
          },
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

void BrowserTool::OnActorTaskStateChanged(actor::ActorTask& task) {
  // custom-browser: Chromium 149's TaskStateChangedCallback delivers ActorTask&;
  // extract id/state to preserve the logic below.
  const actor::TaskId task_id = task.id();
  const actor::ActorTask::State state = task.GetState();
  if (!task_id_.has_value() || task_id != *task_id_) {
    return;
  }

  // Diagnostic: every state transition for our task, to correlate with
  // `tool_dropped` errors. VLOG(1): opt-in via --vmodule=browser_tool=1+.
  VLOG(1) << "[BrowserTool] OnActorTaskStateChanged task_id="
          << task_id.value() << " state=" << static_cast<int>(state)
          << " session=" << GetSessionKey();

  // Terminal states (kFinished / kCancelled / kFailed) are sticky, so drop the
  // cached `task_id_` here: a stale id would make the next `PerformActions`
  // silently drop its callback and surface `tool_dropped`.
  if (actor::ActorTask::IsCompletedState(state)) {
    // VLOG(1) — visible with --vmodule=browser_tool=1+. The state
    // transition to terminal is an important diagnostic but happens
    // on routine cross-origin nav / task-stop paths too, so the
    // default-level INFO floor would make it too chatty.
    VLOG(1) << "[BrowserTool] actor task entered terminal state "
            << static_cast<int>(state) << " session=" << GetSessionKey()
            << "; clearing cached task_id_ so the next tool call recreates";
    for (const scoped_refptr<PausableTimeout>& t : active_timeouts_) {
      t->Stop();
    }
    active_timeouts_.clear();
    actor_task_paused_ = false;
    task_state_subscription_ = {};
    task_id_.reset();
    // node_registry_ is intentionally NOT cleared — element indexes
    // are document-scoped and the user's existing tabs (and their
    // DOM) survive task termination. OnStop is the only path that
    // nukes the registry; a task-level terminal state should leave
    // per-tab state intact.
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
    std::move(done).Reject("no_profile", PhaseParams("task_create"));
    return std::nullopt;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", PhaseParams("task_create"));
    return std::nullopt;
  }
  const auto* policy_checker = GetPolicyChecker(profile);
  if (!policy_checker) {
    std::move(done).Reject("no_policy_checker", PhaseParams("task_create"));
    return std::nullopt;
  }

  actor::webui::mojom::TaskOptionsPtr options =
      actor::webui::mojom::TaskOptions::New();
  options->title = base::StrCat({"nexus-browser-tools:", GetSessionKey()});

  task_id_ = service->CreateTaskWithOptions(
      actor::TaskSourceInfo(actor::TaskSourceInfo::Client::kGlic,
                            /*id=*/std::nullopt),
      policy_checker, std::move(options),
      /*delegate=*/base::WeakPtr<actor::ActorTaskDelegate>(),
      service->GetActorUiStateManager());

  // Post-create validation: the previous task's async cleanup can leave the new
  // task null or not yet kCreated, which would silently drop the next callback;
  // reject with a typed `task_create_*` error instead.
  if (task_id_->is_null()) {
    LOG(WARNING) << "[BrowserTool] EnsureTaskCreated session="
                 << GetSessionKey()
                 << " CreateTaskWithOptions returned null TaskId";
    std::move(done).Reject("task_create_failed", base::DictValue());
    task_id_.reset();
    return std::nullopt;
  }
  actor::ActorTask* new_task = service->GetTask(*task_id_);
  if (!new_task) {
    LOG(WARNING) << "[BrowserTool] EnsureTaskCreated session="
                 << GetSessionKey() << " GetTask(" << task_id_->value()
                 << ") returned null immediately after creation";
    std::move(done).Reject("task_create_unhealthy", ReasonParams("not_registered"));
    task_id_.reset();
    return std::nullopt;
  }
  if (new_task->GetState() != actor::ActorTask::State::kCreated) {
    // Anything other than kCreated immediately after creation is
    // unexpected. kPausedByActor / kPausedByUser / kWaitingOnUser
    // shouldn't happen for a brand-new task. kCancelled / kFinished /
    // kFailed mean the service has already torn it down.
    LOG(WARNING) << "[BrowserTool] EnsureTaskCreated session="
                 << GetSessionKey() << " task " << task_id_->value()
                 << " in unexpected initial state "
                 << static_cast<int>(new_task->GetState());
    {
      base::DictValue reject_params = ReasonParams("bad_state");
      reject_params.Set("state", static_cast<int>(new_task->GetState()));
      std::move(done).Reject("task_create_unhealthy", std::move(reject_params));
    }
    task_id_.reset();
    return std::nullopt;
  }
  // VLOG(1) — successful-task-create diagnostic, opt-in via
  // --vmodule=browser_tool=1+. The failure-path equivalents above
  // stay LOG(WARNING) so they're always visible.
  VLOG(1) << "[BrowserTool] EnsureTaskCreated session=" << GetSessionKey()
          << " new task_id=" << task_id_->value()
          << " state=" << static_cast<int>(new_task->GetState());

  task_state_subscription_ = service->AddTaskStateChangedCallback(
      base::BindRepeating(&BrowserTool::OnActorTaskStateChanged,
                          weak_ptr_factory_.GetWeakPtr()));
  return task_id_;
}

void BrowserTool::RunActorAction(apc::Actions actions,
                                 std::string_view action_name,
                                 ToolCallback done,
                                 std::optional<int32_t> working_tab_id) {
  // Record the tab this dispatch is targeting before the action runs —
  // the next tab-less tool call will use this as its default focus via
  // `LastWorkTab()` if Chrome's foreground tab isn't session-owned.
  // Validated session-owned inside `RecordWorkTab`; nullopt means "no
  // specific working tab" (e.g. close_tab) and we skip the update.
  if (working_tab_id.has_value()) {
    RecordWorkTab(tabs::TabHandle(*working_tab_id));
    WatchJsDialogs(*working_tab_id);
    if (RefuseWhileJsDialogOpen(*working_tab_id, action_name, done)) {
      return;
    }
  }

  // Wrap `done` with a per-action wall-clock budget so a hung
  // CDP/WebSocket can't deadlock the loop. The settle-once timer
  // races the legitimate PerformActions → BuildActionsResultWithObservations
  // → OnActionsResultBuilt completion chain; whichever finishes first
  // resolves the underlying ToolCallback. JS-side step_timeout still
  // protects the whole-step level; this is the finer-grained native
  // safety net for individual actor calls.
  done = PausableBudgetWrap(std::move(done), action_timeout_,
                            std::string(action_name));
  if (working_tab_id.has_value()) {
    done = TrackInFlight(std::move(done), action_name, *working_tab_id);
  }

  std::optional<actor::TaskId> task_id = EnsureTaskCreated(done);
  if (!task_id.has_value()) {
    return;  // EnsureTaskCreated already rejected `done`.
  }
  auto* service = actor::ActorKeyedService::Get(GetProfile());
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "dispatch"));
    return;
  }

  actions.set_task_id(task_id->value());

  actor::BuildToolRequestResult tool_request_result =
      actor::BuildToolRequest(actions);
  if (!tool_request_result.has_value()) {
    std::move(done).Reject("invalid_action", ActionParams(action_name, "build"));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  actor::ActorTaskMetadata task_metadata;

  // Diagnostic: log the dispatch to locate where a callback goes missing when
  // `tool_dropped` reaches the guest. VLOG(1): --vmodule=browser_tool=1+.
  VLOG(1) << "[BrowserTool] PerformActions dispatch action="
          << action_name << " task_id=" << task_id->value()
          << " task_state="
          << static_cast<int>(
                 service->GetTask(*task_id) ?
                     service->GetTask(*task_id)->GetState() :
                     actor::ActorTask::State::kFailed)
          << " session=" << GetSessionKey();

  // Keep a copy for the one-shot offscreen retry (see
  // OnActorPerformActionsDone) — `actions` itself is consumed by
  // BuildToolRequest above.
  service->PerformActions(
      *task_id, std::move(tool_request_result.value()),
      std::move(task_metadata),
      base::BindOnce(&BrowserTool::OnActorPerformActionsDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::string(action_name), start_time,
                     /*actions_for_retry=*/actions, /*retried=*/false));
}

void BrowserTool::OnActorPerformActionsDone(
    ToolCallback done,
    std::string action_name,
    base::TimeTicks start_time,
    apc::Actions actions_for_retry,
    bool retried,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TabObservationStrategy /*observation_strategy*/) {
  // Chromium 149's PerformActions callback no longer passes a separate
  // result_code — it's folded into the per-action `action_results` vector.
  // Reduce to the overall code via upstream's canonical helper (first
  // non-OK action wins); see `OverallActorResultCode` for the rationale.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  // Diagnostic: log entry so we can confirm whether the actor service
  // invoked this callback (vs. dropped the BindOnce on the floor,
  // which would skip this log and surface as `tool_dropped` via the
  // outer ToolCallback's destructor). See the matching dispatch log
  // in RunActorAction.
  //
  // VLOG(1): opt-in via --vmodule=browser_tool=1+. Routine per-tool-
  // call line.
  VLOG(1) << "[BrowserTool] OnActorPerformActionsDone action="
          << action_name
          << " result_code=" << static_cast<int>(result_code)
          << " session=" << GetSessionKey()
          << " task_id="
          << (task_id_.has_value()
                  ? base::NumberToString(task_id_->value())
                  : std::string("(none)"));

  // PerformActions has settled. Hand off to the actor framework's
  // observation builder so the LLM gets a fresh page snapshot back.
  // Without this stage the LLM has no way to know what the action did.
  Profile* profile = GetProfile();
  if (!profile) {
    std::move(done).Reject("no_profile", ActionParams(action_name, "observe"));
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "observe"));
    return;
  }

  // One-shot transient-offscreen retry. A freshly-created same-window tab
  // can be momentarily 0-sized while the panel's agent zone lays out, so
  // the actor's visibility hit-test reports `kElementOffscreen` for an
  // element that is about to become perfectly interactable (the element IS
  // in the APC with a box — only the widget geometry lags). Re-dispatch the
  // SAME actions once after a short settle; a genuinely offscreen element
  // fails again and surfaces the normal error.
  if (result_code == actor::mojom::ActionResultCode::kElementOffscreen &&
      !retried && task_id_.has_value()) {
    LOG(INFO) << "[BrowserTool] " << action_name
              << ": kElementOffscreen on first attempt — retrying once "
                 "after settle (same-window layout race)";
    apc::Actions actions_copy = actions_for_retry;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BrowserTool> self, apc::Actions actions,
               std::string action_name, base::TimeTicks start_time,
               ToolCallback done) {
              if (!self) {
                std::move(done).Reject("tool_gone", ActionParams(action_name));
                return;
              }
              auto* svc = actor::ActorKeyedService::Get(self->GetProfile());
              if (!svc || !self->task_id_.has_value()) {
                std::move(done).Reject("no_actor_service", ActionParams(action_name, "offscreen_retry"));
                return;
              }
              actor::BuildToolRequestResult req =
                  actor::BuildToolRequest(actions);
              if (!req.has_value()) {
                std::move(done).Reject("invalid_action", ActionParams(action_name, "offscreen_retry"));
                return;
              }
              svc->PerformActions(
                  *self->task_id_, std::move(req.value()),
                  actor::ActorTaskMetadata{},
                  base::BindOnce(&BrowserTool::OnActorPerformActionsDone,
                                 self, std::move(done),
                                 std::move(action_name), start_time,
                                 /*actions_for_retry=*/actions,
                                 /*retried=*/true));
            },
            weak_ptr_factory_.GetWeakPtr(), std::move(actions_copy),
            action_name, start_time, std::move(done)),
        base::Milliseconds(500));
    return;
  }
  if (!task_id_.has_value()) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "session"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "actor"));
    return;
  }
  // Auto-recover from a wedged execution engine: code 37 means a phantom prior
  // action blocks every later call until action_timeout. Stop the task, reset
  // the cached id + subscription and return a typed error; the next call's
  // `EnsureTaskCreated` mints a fresh task.
  if (result_code ==
      actor::mojom::ActionResultCode::kExecutionEngineExistingAction) {
    LOG(WARNING) << "[BrowserTool] " << action_name
                 << ": actor engine wedged (code 37); auto-recovering "
                    "by stopping task " << task_id_->value()
                 << " session=" << GetSessionKey();
    service->StopTask(*task_id_,
                      actor::ActorTask::StoppedReason::kShutdown);
    task_state_subscription_ = {};
    task_id_.reset();
    actor_task_paused_ = false;
    for (const scoped_refptr<PausableTimeout>& t : active_timeouts_) {
      t->Stop();
    }
    active_timeouts_.clear();
    std::move(done).Reject("actor_engine_wedged", ActionParams(action_name));
    return;
  }
  // The task map keeps terminal-state tasks, so `GetTask` can return a dead one:
  // reject with a typed `task_went_away` and clear the cached id rather than let
  // the callback drop. Covers the race before the state-change broadcast.
  if (task->IsCompleted()) {
    LOG(WARNING) << "[BrowserTool] " << action_name
                 << ": actor task already in terminal state "
                 << static_cast<int>(task->GetState())
                 << " session=" << GetSessionKey();
    task_state_subscription_ = {};
    task_id_.reset();
    {
      base::DictValue reject_params = ActionParams(action_name);
      reject_params.Set("state", static_cast<int>(task->GetState()));
      std::move(done).Reject("task_went_away", std::move(reject_params));
    }
    return;
  }
  // Skip the observation pipeline when the task has no tabs (e.g. last tab
  // closed mid-batch); upstream crashes have been observed in that path.
  if (task->GetTabs().empty()) {
    std::move(done).Reject("no_tabs", ActionParams(action_name));
    return;
  }

  // custom-browser: Chromium 149 dropped the separate result_code/index args
  // (folded into action_results) and added screenshot_collection_options.
  actor::BuildActionsResultWithObservations(
      *profile, start_time, std::move(action_results), *task,
      /*skip_async_observation_information=*/false,
      /*screenshot_collection_options=*/std::nullopt,
      base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::move(action_name)));
}

void BrowserTool::OnActionsResultBuilt(
    ToolCallback done,
    std::string action_name,
    base::TimeTicks /*start_time*/,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TaskId /*task_id*/,
    bool /*skip_async_observation_information*/,
    std::optional<page_content_annotations::ScreenshotOptions::
                      ScreenshotCollectionOptions>
    /*screenshot_collection_options*/,
    std::unique_ptr<apc::ActionsResult> result_proto,
    std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> /*journal*/) {
  // Chromium 149 folded the result code into the per-action `action_results`
  // vector; reduce it via upstream's canonical helper (first non-OK wins).
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  // Read-and-clear the late-navigation retry latch FIRST, so every exit —
  // ok, actor error, even a failed retry observation — resets it and the
  // NEXT action gets its own guard. One action is in flight per session at
  // a time, so a plain member is a safe carrier.
  const bool was_settle_retry = nav_settle_observation_retried_;
  nav_settle_observation_retried_ = false;
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
    // Only BrowserScreenshot wants the actual image bytes delivered to
    // the model; every other action keeps the lightweight flag.
    observation = BuildObservationDict(
        *result_proto, /*include_screenshot_image=*/action_name == "screenshot");
  } else {
    // No observation this pass — nothing of an earlier walk may be
    // committed below by mistake.
    observation_baseline_.Discard();
  }
  if (was_settle_retry) {
    LOG(INFO) << "[BrowserTool] " << action_name
              << ": settle-guard retry observation built (observed_url="
              << last_observation_url_ << ")";
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
    // The numeric actor code; the runtime's table renders the description
    // (including the tool-aware offscreen / obscured overrides).
    result.Set("error_code", static_cast<int>(result_code));
    // The observation (with its elements[]) rides this error result, so
    // the model does receive it — it is the new T0-c baseline.
    observation_baseline_.Commit();
    std::move(done).Resolve(std::move(result));
    return;
  }

  result.Set("ok", true);

  // ── Late-navigation settle guard ──────────────────────────────────────
  // "Page settled" and a renderer-initiated navigation travel on unordered
  // pipes, so the observation can snapshot the old document. If a navigation is
  // in flight or the URL moved, wait it out (bounded) and re-observe once.
  if (!was_settle_retry && result_proto && result_proto->tabs_size() > 0) {
    const std::optional<tabs::TabHandle> work = LastWorkTab();
    tabs::TabInterface* work_tab = work.has_value() ? work->Get() : nullptr;
    content::WebContents* wc = work_tab ? work_tab->GetContents() : nullptr;
    if (wc) {
      const GURL& committed = wc->GetLastCommittedURL();
      const bool nav_in_flight =
          wc->IsLoading() ||
          wc->GetController().GetPendingEntry() != nullptr;
      const bool url_mismatch = !last_observation_url_.empty() &&
                                GURL(last_observation_url_) != committed;
      if (nav_in_flight || url_mismatch) {
        LOG(INFO) << "[BrowserTool] " << action_name
                  << ": observation raced a navigation (in_flight="
                  << nav_in_flight << " observed_url=" << last_observation_url_
                  << " committed_url=" << committed.spec()
                  << ") — waiting for quiescence, then re-observing once";
        nav_settle_observation_retried_ = true;
        // 300 × 150ms = a 45s bound, inside the 180s per-action budget; the poll exits
        // at commit. The observation built here is only sent if the work tab dies
        // mid-wait; otherwise the re-observe commits its own.
        WaitForWorkTabQuiescenceThenReobserve(
            std::move(done), std::move(action_name), std::move(result),
            /*entry_committed_url=*/committed.spec(),
            /*polls_left=*/300);
        return;
      }
    }
  }

  // Honesty stamp: if we are resolving while the work tab still has a
  // navigation in flight (reachable on the settle-retry path when the
  // bounded wait expired), say so — the model must know the page may be
  // about to change out from under the snapshot it was just handed, and
  // that a fresh `read_page` before the next action is the safe move.
  if (was_settle_retry) {
    const std::optional<tabs::TabHandle> work = LastWorkTab();
    tabs::TabInterface* work_tab = work.has_value() ? work->Get() : nullptr;
    content::WebContents* wc = work_tab ? work_tab->GetContents() : nullptr;
    if (wc && (wc->IsLoading() ||
               wc->GetController().GetPendingEntry() != nullptr)) {
      result.Set("navigation_pending", true);
      result.Set("navigation_pending_hint",
                 "A navigation was still in flight when this observation was "
                 "taken; the page may be about to change. Re-observe with "
                 "read_page before acting on this snapshot.");
    }
  }

  // The elements[] built above ship in this result → they are what the
  // model now holds, so they become the T0-c baseline. (No-op when the
  // walk short-circuited to a stub or produced no working-tab payload.)
  observation_baseline_.Commit();
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::WaitForWorkTabQuiescenceThenReobserve(
    ToolCallback done,
    std::string action_name,
    base::DictValue fallback_result,
    std::string entry_committed_url,
    int polls_left) {
  const std::optional<tabs::TabHandle> work = LastWorkTab();
  tabs::TabInterface* work_tab = work.has_value() ? work->Get() : nullptr;
  content::WebContents* wc = work_tab ? work_tab->GetContents() : nullptr;
  if (!wc) {
    // The work tab died while we waited out its navigation. The ACTION
    // itself succeeded — resolve with the observation we already built
    // rather than failing a successful action over its follow-up snapshot.
    // That observation's projection is still the staged one (no walk ran
    // in between) and it does ship now → commit it.
    observation_baseline_.Commit();
    std::move(done).Resolve(std::move(fallback_result));
    return;
  }
  // Exit as soon as the raced navigation COMMITS (the URL moved off what
  // was committed when the guard armed) — the new document exists and can
  // be observed; waiting further for full load-stop only delays the model
  // behind subresources. Quiescence remains the exit for the
  // mismatch-triggered case where the commit had already happened.
  const bool committed_moved =
      wc->GetLastCommittedURL().spec() != entry_committed_url;
  const bool quiescent = !wc->IsLoading() &&
                         wc->GetController().GetPendingEntry() == nullptr;
  if (!committed_moved && !quiescent && polls_left > 0) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&BrowserTool::WaitForWorkTabQuiescenceThenReobserve,
                       weak_ptr_factory_.GetWeakPtr(), std::move(done),
                       std::move(action_name), std::move(fallback_result),
                       std::move(entry_committed_url), polls_left - 1),
        base::Milliseconds(150));
    return;
  }
  // Committed, quiescent — or still churning after the bounded wait (a
  // long-polling page can hold IsLoading indefinitely). Either way take a
  // FRESH observation: `RunTabObservation` re-marks the work tab, the
  // framework re-fetches APC for the task's tabs, and the result flows
  // through `OnActionsResultBuilt` again — which skips this guard via the
  // latch, so at most one retry per action.
  LOG(INFO) << "[BrowserTool] " << action_name
            << ": settle-guard re-observing (committed_moved="
            << committed_moved << " quiescent=" << quiescent
            << " polls_left=" << polls_left
            << " loading=" << wc->IsLoading()
            << " committed_url=" << wc->GetLastCommittedURL().spec() << ")";
  RunTabObservation(work->raw_value(), action_name, std::move(done),
                    base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                                   weak_ptr_factory_.GetWeakPtr()));
}

// =============================================================================
// Tool bodies: action tools dispatch one `apc::Actions` via `RunActorAction`;
// observation actions use `RunTabObservation`; `read_element` reads the
// element registry synchronously.
// =============================================================================

}  // namespace nexus

