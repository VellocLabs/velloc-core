#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/actor.mojom.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"
#include "custom_browser/browser/nexus/tools/browser_tool/screenshot_shaping.h"

namespace nexus {

// File-local shorthand: same `using` directive browser_tool.cc uses,
// so call sites stay unqualified across the BrowserTool .cc files.
using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

// =============================================================================
// BrowserScreenshot capture + shaping.
// =============================================================================

// The capture BrowserScreenshot asks upstream for: lossless PNG at native
// resolution. No `max_width` / `max_height` on purpose — element boxes are in
// captured device pixels, so the crop must run on the unscaled image; the
// long-edge cap is applied afterwards by `ShapeScreenshotResult`.
std::optional<page_content_annotations::ScreenshotOptions::
                  ScreenshotCollectionOptions>
ModelScreenshotCaptureOptions() {
  page_content_annotations::ScreenshotOptions::ScreenshotCollectionOptions
      options;
  options.screenshot_image_format =
      page_content_annotations::ScreenshotOptions::ScreenshotImageFormat::kPng;
  // Anything but kLow selects the full-compression PNG encoder upstream.
  options.screenshot_compression_quality = page_content_annotations::
      ScreenshotOptions::ScreenshotCompressionQuality::kHigh;
  return options;
}

// Wraps `inner` so a successful BrowserScreenshot result is shaped on the
// thread pool (decode / crop / resize / encode is CPU work that must stay off
// the UI thread) before it resolves. Errors pass straight through. The two
// callbacks share `inner` through a ref-counted holder; if the wrapper or the
// reply is dropped unfired, `inner`'s own destructor reports the drop.
ToolCallback WrapWithScreenshotShaping(ToolCallback inner,
                                       std::optional<int32_t> crop_index) {
  auto holder = base::MakeRefCounted<base::RefCountedData<ToolCallback>>(
      std::move(inner));
  return ToolCallback(
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<ToolCallback>> h,
             std::optional<int32_t> index, base::Value value) {
            if (!value.is_dict()) {
              std::move(h->data).Resolve(std::move(value));
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE,
                {base::TaskPriority::USER_BLOCKING,
                 base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
                base::BindOnce(&ShapeScreenshotResult,
                               std::move(value).TakeDict(), index,
                               kScreenshotMaxEdge),
                base::BindOnce(
                    [](ToolCallback done, base::DictValue shaped) {
                      std::move(done).Resolve(std::move(shaped));
                    },
                    std::move(h->data)));
          },
          holder, crop_index),
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<ToolCallback>> h,
             std::string code, std::string /*message*/,
             base::DictValue params) {
            std::move(h->data).Reject(std::move(code), std::move(params));
          },
          holder));
}

}  // namespace

// =============================================================================
// Private helper: RunCreateTabAction, for `RunNavigateOnUI` (new_tab=true) and
// `RunSearch`; always opens a new tab.
// =============================================================================

void BrowserTool::RunCreateTabAction(std::string url,
                                     std::string action_name,
                                     ToolCallback done) {
  // No `url.empty()` guard — empty required strings are rejected by
  // the codegen-emitted Extract() before this body runs.
  Profile* profile = GetProfile();

  // ---- New-tab path ----------------------------------------------------
  // CreateTabAction.window_id is required by `CreateCreateTabRequest`
  // (`!action.has_window_id()` returns nullptr → "failed to build
  // ToolRequest"), so resolve the target window's SessionID up front.
  // ResolveCreateTabWindowSessionId honors the
  // `nexus.browser_tools_separate_window` pref: when off it returns the
  // active (Nexus) window; when on it returns a dedicated agent window,
  // creating one on demand.
  std::optional<int32_t> active_window =
      ResolveCreateTabWindowSessionId(profile);
  if (!active_window.has_value()) {
    std::move(done).Reject("no_active_window", ActionParams(action_name));
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
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "unavailable"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "actor"));
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
    std::move(done).Reject("invalid_action", ActionParams(action_name, "create_tab"));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  service->PerformActions(
      *task_id, std::move(tool_request_result.value()),
      actor::ActorTaskMetadata{},
      base::BindOnce(&BrowserTool::OnCreateTabActionDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::move(url), std::move(action_name),
                     std::move(tabs_before), start_time));
}

void BrowserTool::OnCreateTabActionDone(
    ToolCallback done,
    std::string url,
    std::string action_name,
    actor::ActorTask::TabHandleSet tabs_before,
    base::TimeTicks /*start_time*/,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TabObservationStrategy /*observation_strategy*/) {
  // 149 folded the standalone result_code / index_of_failed_action out of
  // PerformActionsCallback into the per-action `action_results` vector;
  // reduce it via upstream's canonical helper (first non-OK wins). This
  // CreateTab request is single-action, so the result is just its code.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  if (!actor::IsOk(result_code)) {
    {
      base::DictValue reject_params = ActionParams(action_name);
      reject_params.Set("code", static_cast<int>(result_code));
      std::move(done).Reject("create_tab_failed", std::move(reject_params));
    }
    return;
  }
  Profile* profile = GetProfile();
  if (!profile || !task_id_.has_value()) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "navigate"));
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "navigate"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "navigate_actor"));
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
    std::move(done).Reject("tab_not_observed", ActionParams(action_name));
    return;
  }

  // Bind the new tab to this Nexus session before issuing the navigate.
  // BrowserTool::BindTabToSession attaches the WebContents tag (source
  // of truth), adds the tab to this session's tab group (creating the
  // group lazily on the first session tab), records membership in
  // `session_tabs_`, and marks it as last-active. No-op when the
  // feature flag is off or the session key is empty.
  BindTabToSession(*new_tab);

  // Stage 2 — navigate the new tab through `RunActorAction`, with a trailing
  // `WaitAction`: the snapshot otherwise fires at initial commit and can catch
  // an empty about:blank transition.
  apc::Actions nav_actions;
  apc::NavigateAction* nav = nav_actions.add_actions()->mutable_navigate();
  nav->mutable_url()->assign(url);
  nav->set_tab_id(new_tab->raw_value());

  apc::WaitAction* wait = nav_actions.add_actions()->mutable_wait();
  wait->set_wait_time_ms(2000);
  wait->set_observe_tab_id(new_tab->raw_value());

  // The freshly-created tab IS the working tab for this stage-2
  // navigate — emit its full elements in the observation.
  RunActorAction(std::move(nav_actions), action_name, std::move(done),
                  new_tab->raw_value());
}

// =============================================================================
// Observation runner. Bypasses the actor `PerformActions` path entirely:
// marks the target tab as "observe-once" on the actor task, then calls
// `BuildActionsResultWithObservations` directly with no actions — the
// framework still fetches APC + screenshot for the marked tabs because
// it walks `task->GetLastActedTabs()`, into which `ObserveTabOnce`
// inserted the tab.
// =============================================================================

void BrowserTool::RunTabObservation(int32_t tab_id_value,
                                     std::string_view action_name,
                                     ToolCallback done,
                                     ObservationCompletion completion) {
  // Observation-only actions still operate on a specific tab — record
  // it as the most-recent worked-on tab so a subsequent tab-less call
  // can default to it via `LastWorkTab()` when Chrome's foreground
  // tab isn't session-owned. Mirrors `RunActorAction`.
  RecordWorkTab(tabs::TabHandle(tab_id_value));
  WatchJsDialogs(tab_id_value);
  if (RefuseWhileJsDialogOpen(tab_id_value, action_name, done)) {
    return;
  }

  // Same per-action budget the actor-driven path uses — observation-
  // only actions (screenshot, read_page, find_text) also dispatch
  // through the actor framework and can hang on a stalled APC fetch.
  done = PausableBudgetWrap(std::move(done), action_timeout_,
                            std::string(action_name));
  done = TrackInFlight(std::move(done), action_name, tab_id_value);

  Profile* profile = GetProfile();
  if (!profile) {
    std::move(done).Reject("no_profile", ActionParams(action_name, "tab"));
    return;
  }
  std::optional<actor::TaskId> task_id_opt = EnsureTaskCreated(done);
  if (!task_id_opt.has_value()) {
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "unavailable"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_opt);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "actor"));
    return;
  }

  // Validate the target tab is live before handing it to the actor task.
  // ObserveTabOnce silently no-ops on a dead handle, but a silent no-op
  // would leave the upstream pipeline walking only `controlled_tabs_`
  // (potentially a stale carryover from a prior action whose
  // ActorTabData::last_observed_page_content_ is unsafe to overwrite),
  // which is the crash window described at the
  // OnActorPerformActionsDone guard.
  tabs::TabHandle target_handle(tab_id_value);
  if (!target_handle.Get()) {
    std::move(done).Reject("tab_gone", ActionParams(action_name));
    return;
  }
  task->ObserveTabOnce(target_handle);

  // BuildActionsResultWithObservations walks `task->GetLastActedTabs()`
  // (= controlled_tabs_ ∪ to_observe_tabs_), not `GetTabs()` — so the
  // empty-set guard mirrored from OnActorPerformActionsDone has to be
  // checked against that union here.
  if (task->GetLastActedTabs().empty()) {
    std::move(done).Reject("no_tabs", ActionParams(action_name));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  // Chromium 149 signature: no prior action results here, so an empty vector.
  // Screenshot options only for BrowserScreenshot, the one action whose pixels
  // reach the model.
  actor::BuildActionsResultWithObservations(
      *profile, start_time,
      /*action_results=*/{},
      *task,
      /*skip_async_observation_information=*/false,
      /*screenshot_collection_options=*/
      action_name == "screenshot" ? ModelScreenshotCaptureOptions()
                                  : std::nullopt,
      base::BindOnce(std::move(completion), std::move(done),
                     std::string(action_name)));
}

// =============================================================================
// LLM-facing tool methods on the common path; the rarer actions live in
// browser_tool_{page_text,file_transfer,evaluate_action,media_actions,
// fill_form}.cc. Each is the single entry point
// for one declarative *.tool.json. The codegen-emitted dispatcher
// trampoline in nexus_tools_registry.gen.cc routes here with typed
// input pre-extracted from the LLM's tool_use args.
// =============================================================================

void BrowserTool::RunScreenshotOnUI(::nexus::generated::BrowserScreenshotInput input,
                                    ToolCallback done) {
  // Phase 13: schema no longer exposes tab_id — always target the
  // currently-active tab. Cross-tab screenshots require BrowserActivateTab
  // first.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "screenshot", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // Shape the image (optional element crop, long-edge cap, PNG) on its way
  // out — see screenshot_shaping.h. Wrapping `done` covers every resolve
  // site of `OnActionsResultBuilt` (ok, actor error, settle-guard fallback)
  // in one place.
  done = WrapWithScreenshotShaping(std::move(done), input.index);
  // Reuse the standard `OnActionsResultBuilt`: it routes the `"screenshot"`
  // action name to `BuildObservationDict(include_screenshot_image=true)`, so
  // the working tab's dict carries the actual `screenshot: {mime,data_base64}`
  // image (every other action omits it). The `action_name == "screenshot"`
  // gate downstream is what distinguishes this path.
  RunTabObservation(
      *resolved_tab_id, "screenshot", std::move(done),
      base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                     weak_ptr_factory_.GetWeakPtr()));
}

void BrowserTool::RunNavigateOnUI(::nexus::generated::BrowserNavigateInput input,
                                  ToolCallback done) {
  // new_tab=true opens via `RunCreateTabAction` with action_name "navigate";
  // otherwise the active tab navigates in place.
  if (input.new_tab.value_or(false)) {
    RunCreateTabAction(std::move(input.url),
                       /*action_name=*/"navigate",
                       std::move(done));
    return;
  }

  // First-tab auto-promotion: with no session tab yet, route through the
  // new-tab path instead of rejecting with `no_session_tab`.
  if (CollectSessionTabs().empty()) {
    LOG(INFO) << "[BrowserTool] navigate: session owns no tabs; "
                 "auto-promoting to new_tab=true";
    RunCreateTabAction(std::move(input.url),
                       /*action_name=*/"navigate",
                       std::move(done));
    return;
  }

  // ---- In-place navigation on the active tab ---------------------------
  // The actor's CreateNavigateRequest rejects if `tab_id` is unset
  // (`tab_handle == TabHandle::Null()`), so we must resolve the active
  // tab handle and emit it explicitly.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "navigate", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::NavigateAction* navigate = actions.add_actions()->mutable_navigate();
  navigate->mutable_url()->assign(input.url);
  navigate->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "navigate", std::move(done),
                  *resolved_tab_id);
}

void BrowserTool::RunBackOnUI(::nexus::generated::BrowserBackInput input,
                              ToolCallback done) {
  // Phase 13: schema dropped tab_id; always act on the active tab.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "back", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::HistoryBackAction* back = actions.add_actions()->mutable_back();
  back->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "back", std::move(done),
                  *resolved_tab_id);
}

// Phase 13: `RunForwardOnUI` retired alongside the `forward` tool. The
// forward-navigation use case was rare enough in agent workflows that
// keeping the surface area was net-negative; if a model goes back and
// then wants to "return", it can `BrowserNavigate(<original_url>)`.

void BrowserTool::RunCloseTabOnUI(::nexus::generated::BrowserCloseTabInput input,
                                  ToolCallback done) {
  // close_tab is one of two tools that KEEP `tab_id` (the other is
  // activate_tab). Both intentionally let the model target a non-active
  // tab — closing a background popup is the canonical use.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(input.tab_id, GetProfile(), "close_tab", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // The tab's document is going away with it: evict its registry entries
  // and its per-tab document record now (they are otherwise only pruned on
  // OnStop). Done before the action rather than in its callback so a
  // failed close costs at most one "stale index" re-observe for that tab.
  node_registry_.ClearTab(*resolved_tab_id);
  registry_document_by_tab_.erase(*resolved_tab_id);
  apc::Actions actions;
  apc::CloseTabAction* close_tab =
      actions.add_actions()->mutable_close_tab();
  close_tab->set_tab_id(*resolved_tab_id);
  // No working_tab_id — the targeted tab is being closed. Post-close
  // observation surfaces every surviving session-owned tab as basics
  // only. The next tab-less call falls back to LastWorkTab() if the
  // closed tab was the recorded one, or to first session-owned tab.
  RunActorAction(std::move(actions), "close_tab", std::move(done),
                  std::nullopt);
}

void BrowserTool::RunActivateTabOnUI(::nexus::generated::BrowserActivateTabInput input,
                                     ToolCallback done) {
  // ActivateTabInput.tab_id is required (bare int32_t, not optional);
  // the codegen-emitted Extract() rejects a missing key with
  // `invalid_args` before this body runs (see activate_tab.gen.h).
  //
  // Route through ResolveTabIdOrReject so the per-session check
  // (kSessionScopedTabs) gets applied uniformly — without it, the LLM
  // could activate a tab from another session simply by knowing its id.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(input.tab_id, GetProfile(), "activate_tab", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // No stored "last active tab" — after this ActivateTabAction dispatches,
  // Chrome's `GetActiveTabHandleId(profile)` will return *resolved_tab_id
  // on the next call, which is what tab-less tool calls fall back to via
  // `GetSessionScopedActiveTabHandleId`.
  apc::Actions actions;
  apc::ActivateTabAction* activate_tab =
      actions.add_actions()->mutable_activate_tab();
  activate_tab->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "activate_tab", std::move(done),
                  *resolved_tab_id);
}

// Phase 13: `RunWaitOnUI` retired alongside the `wait` tool. The
// framework's actor pipeline already settles between actions; an
// explicit LLM-facing wait knob was used so rarely it was net-negative.
// If a specific page genuinely needs an extra settle, that's a bug in
// the actor pipeline to fix — not an LLM-facing parameter.

void BrowserTool::RunClickOnUI(::nexus::generated::BrowserClickInput input,
                               ToolCallback done) {
  // Phase 13: dropped tab_id (always the index's home tab), button
  // (always LEFT), and click_count (always SINGLE). Right-click /
  // double/triple click were so rarely used by LLMs that the surface
  // was net-negative; dedicated tools can ship later if needed.
  if (JsDialogWatcher* watcher =
          IsJsDialogIndex(input.index) ? FindOpenJsDialog() : nullptr) {
    std::optional<bool> accept =
        JsDialogAcceptForIndex(*watcher->open_dialog(), input.index);
    if (accept.has_value()) {
      AnswerJsDialog(watcher, *accept, std::move(done));
    } else {
      RefuseWhileJsDialogOpen(watcher->tab_id(), "click", done);
    }
    return;
  }
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "click");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(entry->content_node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);
  click->set_click_type(apc::ClickAction::LEFT);
  click->set_click_count(apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "click", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunInputOnUI(::nexus::generated::BrowserInputInput input,
                               ToolCallback done) {
  // Phase 13: dropped tab_id. The index's home tab is the implicit
  // target — if it's not the active tab, BrowserActivateTab(N) first.
  if (input.index == kJsDialogTextIndex) {
    JsDialogWatcher* watcher = FindOpenJsDialog();
    if (watcher && watcher->open_dialog()->type == "prompt") {
      if (input.submit.value_or(false)) {
        watcher->open_dialog()->prompt_text = std::move(input.text);
        AnswerJsDialog(watcher, /*accept=*/true, std::move(done));
      } else {
        StageJsDialogText(std::move(input.text), std::move(done));
      }
      return;
    }
  }
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "input");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::TypeAction* type = actions.add_actions()->mutable_type();
  type->mutable_target()->set_content_node_id(entry->content_node_id);
  type->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  type->set_tab_id(entry->tab_id);
  type->set_text(std::move(input.text));
  // The actor's TypeAction currently only supports DELETE_EXISTING (the
  // text replaces existing content). The JS spec also documents this
  // as the contract for input.
  type->set_mode(apc::TypeAction::DELETE_EXISTING);
  type->set_follow_by_enter(input.submit.value_or(false));
  RunActorAction(std::move(actions), "input", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunSelectOnUI(::nexus::generated::BrowserSelectInput input,
                                ToolCallback done) {
  // Phase 13: dropped tab_id. Uses the registry's home tab for the index.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "select");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::SelectAction* select = actions.add_actions()->mutable_select();
  select->mutable_target()->set_content_node_id(entry->content_node_id);
  select->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  select->set_tab_id(entry->tab_id);
  select->set_value(std::move(input.value));
  RunActorAction(std::move(actions), "select", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunHoverOnUI(::nexus::generated::BrowserHoverInput input,
                               ToolCallback done) {
  // Phase 13: dropped tab_id. Uses the registry's home tab for the index.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "hover");
  if (!entry) {
    return;
  }
  apc::Actions actions;
  apc::MoveMouseAction* move = actions.add_actions()->mutable_move_mouse();
  move->mutable_target()->set_content_node_id(entry->content_node_id);
  move->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  move->set_tab_id(entry->tab_id);
  RunActorAction(std::move(actions), "hover", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunDragAndDropOnUI(::nexus::generated::BrowserDragAndDropInput input,
                                     ToolCallback done) {
  // Phase 13: dropped tab_id. Both indices must be on the same tab —
  // checked below — and that tab is always the registry's home tab
  // for from_index.
  const NodeRegistry::Entry* from_entry =
      LookupNode(input.from_index, std::nullopt, done, "drag_and_drop");
  if (!from_entry) {
    return;
  }
  const NodeRegistry::Entry* to_entry =
      LookupNode(input.to_index, std::nullopt, done, "drag_and_drop");
  if (!to_entry) {
    return;
  }
  if (from_entry->tab_id != to_entry->tab_id) {
    // The actor's DragAndReleaseAction takes a single tab_id and
    // operates within one renderer; allowing different home tabs for
    // the two endpoints would silently drop one of them. Reject up
    // front with a clear message instead.
    {
      base::DictValue reject_params = ReasonParams("different_tabs");
      reject_params.Set("from_index", input.from_index);
      reject_params.Set("from_tab", from_entry->tab_id);
      reject_params.Set("to_index", input.to_index);
      reject_params.Set("to_tab", to_entry->tab_id);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }
  apc::Actions actions;
  apc::DragAndReleaseAction* drag =
      actions.add_actions()->mutable_drag_and_release();
  drag->mutable_from_target()->set_content_node_id(from_entry->content_node_id);
  drag->mutable_from_target()
      ->mutable_document_identifier()
      ->set_serialized_token(from_entry->document_id);
  drag->mutable_to_target()->set_content_node_id(to_entry->content_node_id);
  drag->mutable_to_target()
      ->mutable_document_identifier()
      ->set_serialized_token(to_entry->document_id);
  drag->set_tab_id(from_entry->tab_id);
  RunActorAction(std::move(actions), "drag_and_drop", std::move(done),
                  from_entry->tab_id);
}

void BrowserTool::RunScrollToOnUI(::nexus::generated::BrowserScrollToInput input,
                                  ToolCallback done) {
  // Phase 13: dropped tab_id. Uses the registry's home tab for the index.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "scroll_to");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::ScrollToAction* scroll_to =
      actions.add_actions()->mutable_scroll_to();
  scroll_to->mutable_target()->set_content_node_id(entry->content_node_id);
  scroll_to->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  scroll_to->set_tab_id(entry->tab_id);
  RunActorAction(std::move(actions), "scroll_to", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunScrollOnUI(::nexus::generated::BrowserScrollInput input,
                                ToolCallback done) {
  // `amount` is in viewport units (default 1.0), converted to pixels with an
  // assumed viewport size.
  constexpr float kAssumedViewportHeightPx = 800.0f;
  constexpr float kAssumedViewportWidthPx = 1200.0f;
  const float amount = static_cast<float>(input.amount.value_or(1.0));

  int32_t resolved_tab_id = 0;
  const NodeRegistry::Entry* entry = nullptr;
  // Treat `index: 0` as "no element target" (viewport scroll). APC content-
  // node ids start above zero, so 0 is never a real element. Some LLMs
  // emit `0` instead of omitting the field for optional integers
  // (it's a JSON-encoder/sentinel default they fall back to), which used
  // to trap them in a retry loop: `LookupNode(0)` rejected with
  // `index_not_found`, the model re-sent the same shape, and the
  // conversation wedged because nothing else changed. Coerce here.
  const bool has_target_index =
      input.index.has_value() && *input.index != 0;
  if (has_target_index) {
    entry = LookupNode(*input.index, std::nullopt, done, "scroll");
    if (!entry) {
      return;
    }
    resolved_tab_id = entry->tab_id;
  } else {
    // Viewport scroll — the actor's `BuildToolRequest` requires a
    // tab_id on every ScrollAction even with no node target. Always
    // resolve to the active tab.
    std::optional<int32_t> resolved =
        ResolveTabIdOrReject(std::nullopt, GetProfile(), "scroll", done, this);
    if (!resolved.has_value()) {
      return;
    }
    resolved_tab_id = *resolved;
  }

  apc::Actions actions;
  apc::ScrollAction* scroll = actions.add_actions()->mutable_scroll();
  if (entry != nullptr) {
    scroll->mutable_target()->set_content_node_id(entry->content_node_id);
    scroll->mutable_target()
        ->mutable_document_identifier()
        ->set_serialized_token(entry->document_id);
  }
  scroll->set_tab_id(resolved_tab_id);

  // Map the LLM-facing direction string to the actor's enum. Reject
  // values that aren't in the .tool.json's enumerated set (the
  // codegen extractor doesn't enforce string enums today, so this
  // body is the live check).
  float pixel_distance = 0.0f;
  if (input.direction == "up") {
    scroll->set_direction(apc::ScrollAction::UP);
    pixel_distance = amount * kAssumedViewportHeightPx;
  } else if (input.direction == "down") {
    scroll->set_direction(apc::ScrollAction::DOWN);
    pixel_distance = amount * kAssumedViewportHeightPx;
  } else if (input.direction == "left") {
    scroll->set_direction(apc::ScrollAction::LEFT);
    pixel_distance = amount * kAssumedViewportWidthPx;
  } else if (input.direction == "right") {
    scroll->set_direction(apc::ScrollAction::RIGHT);
    pixel_distance = amount * kAssumedViewportWidthPx;
  } else {
    {
      base::DictValue reject_params = ReasonParams("scroll_direction");
      reject_params.Set("value", input.direction);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }

  scroll->set_distance(pixel_distance);
  RunActorAction(std::move(actions), "scroll", std::move(done),
                  resolved_tab_id);
}

void BrowserTool::RunSearchOnUI(::nexus::generated::BrowserSearchInput input,
                                ToolCallback done) {
  // No `input.query.empty()` guard — the codegen-emitted Extract()
  // already rejects an empty required string with `invalid_args`
  // before this body runs (see search.gen.h).

  // Engine → results-page URL template. Each template uses `q` as the
  // query placeholder so `base::ReplaceFirstSubstringAfterOffset` keeps
  // the wiring uniform. The set is intentionally small — the LLM can
  // always fall back to `navigate` for arbitrary URLs.
  std::string engine_id = input.engine.value_or("google");
  // Lowercase ASCII fold so "Baidu" / "BAIDU" / "baidu" all match.
  std::string engine_lower;
  engine_lower.reserve(engine_id.size());
  for (char c : engine_id) {
    engine_lower.push_back((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
  }
  std::string url;
  const std::string encoded =
      base::EscapeQueryParamValue(input.query, /*use_plus=*/true);
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
  // Search always opens results in a new tab; action_name "search" keeps the
  // result's `action` the user-facing tool name.
  RunCreateTabAction(std::move(url),
                     /*action_name=*/"search",
                     std::move(done));
}

// Phase 13: `RunCreateTabOnUI` retired alongside the `create_tab` tool.
// The capability lives on inside `RunNavigateOnUI` (new_tab=true path),
// which routes through `RunCreateTabAction` — the same helper this
// retired override used to call.

void BrowserTool::RunDoneOnUI(::nexus::generated::BrowserDoneInput input,
                              ToolCallback done) {
  // `done` resolves the per-call envelope with `is_done: true`; the
  // JS-side agent loop reads that flag to terminate.
  base::DictValue result;
  result.Set("ok", true);
  result.Set("is_done", true);
  result.Set("success", input.success);
  result.Set("text", std::move(input.text));
  // ToolCallback has overloads for DictValue / ListValue / Value, so
  // we pass the DictValue directly without wrapping in a base::Value.
  std::move(done).Resolve(std::move(result));
}

}  // namespace nexus
