#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/aggregated_journal.h"
#include "chrome/common/actor.mojom-forward.h"
#include "chrome/common/actor/action_result.h"
#include "chrome/common/actor/task_id.h"
#include "components/optimization_guide/proto/features/actions_data.pb.h"
#include "components/optimization_guide/proto/features/common_quality_data.pb.h"
#include "custom_browser/browser/nexus/tools/node_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_macros.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

class Profile;

namespace nexus {

class PausableTimeout;
class ToolSession;  // for CategoryAccessor specialization below

// Per-LLM-session class that owns the browser-actor task and hosts every
// `browser_*` tool method. One instance per session, constructed as a
// member of `ToolSession`.
//
// The `task_id_` member is per-instance: on the first actor-driven
// tool call, `EnsureTaskCreated` mints an `actor::TaskId` via
// `actor::ActorKeyedService` and stashes it here. `OnStop(reason)`
// stops the task and resets `task_id_` so the cleanup is idempotent.
class BrowserTool : public ToolBase {
 public:
  BrowserTool(Profile* profile, const std::string& session_key);
  ~BrowserTool() override;

  BrowserTool(const BrowserTool&) = delete;
  BrowserTool& operator=(const BrowserTool&) = delete;

  // Schema declarations — exports one native LLM tool, browser_actions.
  // Primitive browser operations stay as internal action items so the
  // Browser Use-style coordinator keeps control of batching, page-change
  // guards, loop metadata, and forced done behavior.
  static void DefineTools(ToolRegistry& registry);

  // Notification override — stops the per-session actor task. Called
  // by the cascade through `ToolSession::OnStop(reason)`. Idempotent:
  // safe to call more than once.
  void OnStop(const std::string& reason) override;

  // Per-action wall-clock budget propagated from `ToolSession`.
  // Read by `RunActorAction` / `RunTabObservation` to wrap every
  // outgoing actor call with a settle-once timer guard so a hung
  // CDP/WebSocket can't deadlock the loop. Default 180s; clamped
  // 1s..600s by `ToolSession::set_action_timeout`. Setter is called
  // from ToolSession; not exposed for external mutation.
  void set_action_timeout(base::TimeDelta timeout) { action_timeout_ = timeout; }
  base::TimeDelta action_timeout() const { return action_timeout_; }
  void set_step_timeout(base::TimeDelta timeout) { step_timeout_ = timeout; }
  base::TimeDelta step_timeout() const { return step_timeout_; }

  // Last-observation snapshot used by ToolsAdapter to build the
  // `agent_meta` envelope it appends to every tool result. Populated
  // from inside `BuildObservationDict` / `WalkContentNode` so each
  // tool method's completion path picks up the freshest values
  // automatically. Public so `ToolsAdapter::ForwardSuccessWithAgentMeta`
  // can read them per-call without needing friendship.
  //
  // `last_normalized_projection_` is a deterministic serialization of
  // the rich element payload (type / text / role / form-state / href
  // in walk order) with non-identity bookkeeping (tab_id,
  // document_id) excluded. SHA-256 prefix of this string is the
  // dom_hash used by the JS-side stagnation detector. index is
  // stable for the document's lifetime, so an unchanged interactive
  // surface always produces the same hash.
  const std::string& last_observation_url() const {
    return last_observation_url_;
  }
  const std::string& last_observation_title() const {
    return last_observation_title_;
  }
  int last_observation_interactive_count() const {
    return last_observation_interactive_count_;
  }
  std::optional<int> last_observation_tab_id() const {
    return last_observation_tab_id_;
  }
  const std::string& last_observation_normalized_projection() const {
    return last_observation_normalized_projection_;
  }
  // Focus target may be absent (no focused element); the bool flag
  // separates "no focus" from "focus present but with empty id".
  bool has_last_observation_focus_target() const {
    return last_observation_focus_target_present_;
  }
  const std::string& last_observation_focus_target_id() const {
    return last_observation_focus_target_id_;
  }

  // ----------------------------------------------------------------
  // Model-facing tool method. The LLM calls this once per observed
  // browser state with Browser Use-style step fields plus actions[].
  // ----------------------------------------------------------------

  // Reflection fields are advisory bookkeeping for the model — they
  // have no C++-side semantic meaning and are declared OPTIONAL in
  // the schema (see DefineTools). `actions` is REQUIRED by contract
  // (1-5 items per call) but declared OPTIONAL at the extraction
  // layer so a malformed call (missing key, empty array) is
  // soft-recovered into a no-op `wait`; the body emits a `warnings[]`
  // entry naming the specific slip so the model can correct it next
  // turn. The signature uses `std::optional<base::ListValue>` so
  // missing-key extraction returns nullopt instead of erroring.
  void browser_actions(std::optional<std::string> evaluation_previous_goal,
                       std::optional<std::string> memory,
                       std::optional<std::string> next_goal,
                       std::optional<base::ListValue> actions,
                       ToolCallback done);

 private:
  // ----------------------------------------------------------------
  // Internal action implementations. These intentionally are not
  // registered as native LLM tools; browser_actions dispatches to them
  // after validating one action item from actions[].
  // ----------------------------------------------------------------

  // Page-action helpers. Every element-targeting helper takes the LLM-
  // facing `index` (== APC content_node_id) and an optional
  // `tab_id` for multi-tab disambiguation. Single-tab flows can
  // omit tab_id; the registry resolves to the most-recently-seen
  // tab for that index.
  void RunClickAction(int node_id,
                      std::optional<int> tab_id,
                      std::optional<std::string> button,
                      std::optional<std::string> click_count,
                      ToolCallback done);
  void RunInputAction(int node_id,
                      std::optional<int> tab_id,
                      std::string text,
                      std::optional<bool> submit,
                      ToolCallback done);
  void RunSelectAction(int node_id,
                       std::optional<int> tab_id,
                       std::string value,
                       ToolCallback done);
  // Mouse-only positioning. Useful for surfacing hover-only menus
  // (e.g. nav dropdowns), CSS `:hover`-driven UI, video-control
  // overlays, etc. — anything where revealing the next interaction
  // requires the cursor to land on an element first.
  void RunHoverAction(int node_id,
                      std::optional<int> tab_id,
                      ToolCallback done);
  // Press the mouse on `from_index`, drag, and release on
  // `to_index`. Both indexes must be from the same tab; the actor
  // framework has no cross-tab drag.
  void RunDragAndDropAction(int from_node_id,
                            int to_node_id,
                            std::optional<int> tab_id,
                            ToolCallback done);
  void RunNavigateAction(std::optional<int> tab_id,
                         std::string url,
                         ToolCallback done);
  void RunScrollToAction(int node_id,
                         std::optional<int> tab_id,
                         ToolCallback done);
  void RunScrollAction(std::optional<int> node_id,
                       std::optional<int> tab_id,
                       std::string direction,
                       double distance,
                       ToolCallback done);
  void RunWaitAction(std::optional<int> tab_id,
                     std::optional<int> wait_ms,
                     ToolCallback done);
  void RunBackAction(std::optional<int> tab_id, ToolCallback done);
  void RunForwardAction(std::optional<int> tab_id, ToolCallback done);
  void RunCreateTabAction(std::string url,
                          std::optional<bool> reuse_visible_tab,
                          ToolCallback done);
  void RunSearchAction(std::string query,
                       std::optional<std::string> engine,
                       ToolCallback done);
  void RunCloseTabAction(std::optional<int> tab_id, ToolCallback done);
  void RunActivateTabAction(int tab_id, ToolCallback done);

  // MediaSession controls for tabs hosting <video>/<audio> that have
  // registered a media session via the MediaSession Web API. These
  // drive the page-level transport without finding the on-screen
  // play/pause/seek controls; they fail (gracefully) on tabs without
  // an active media session.
  void RunMediaPlayAction(std::optional<int> tab_id, ToolCallback done);
  void RunMediaPauseAction(std::optional<int> tab_id, ToolCallback done);
  void RunMediaSeekAction(std::optional<int> tab_id,
                          int seek_time_ms,
                          ToolCallback done);
  // Read the tab's MediaSession snapshot — playback state, current
  // position, total duration, playback rate, and the
  // navigator.mediaSession.metadata-supplied title/artist/album.
  // Synchronous read; does NOT go through the actor pipeline.
  void RunMediaStateAction(std::optional<int> tab_id, ToolCallback done);

  // Observation actions — read-only; bypass the actor pipeline.
  void RunScreenshotAction(std::optional<int> tab_id, ToolCallback done);
  void RunReadPageAction(std::optional<int> tab_id,
                         std::optional<std::string> scope,
                         std::optional<int> max_chars,
                         ToolCallback done);
  void RunReadElementAction(int node_id,
                            std::optional<int> tab_id,
                            std::optional<int> max_chars,
                            ToolCallback done);
  void RunFindTextAction(std::optional<int> tab_id,
                         std::string query,
                         std::optional<int> max_results,
                         ToolCallback done);

  struct BrowserActionBatchState;

  void RunBrowserActionBatch(scoped_refptr<BrowserActionBatchState> state,
                             size_t index);
  void RunBrowserActionItem(const base::DictValue& action,
                            std::string_view action_type,
                            ToolCallback done);
  void OnBrowserActionItemSuccess(scoped_refptr<BrowserActionBatchState> state,
                                  size_t index,
                                  std::string action_type,
                                  std::optional<int> preferred_tab_id,
                                  std::optional<int> pre_action_tab_id,
                                  std::string pre_action_url,
                                  bool pre_action_focus_present,
                                  std::string pre_action_focus_target_id,
                                  base::Value result);
  void OnBrowserActionItemError(scoped_refptr<BrowserActionBatchState> state,
                                size_t index,
                                std::string action_type,
                                std::string code,
                                std::string message);
  void OnBrowserActionsStepTimeout(
      scoped_refptr<BrowserActionBatchState> state);
  void ResolveBrowserActions(scoped_refptr<BrowserActionBatchState> state,
                             base::DictValue result);

  // Lazily create the per-instance `actor::TaskId` on first
  // actor-driven tool call. Returns std::nullopt and rejects `done`
  // if the actor service / policy checker is unavailable.
  std::optional<actor::TaskId> EnsureTaskCreated(ToolCallback& done);

  // Builds the `actor::Actions` proto's task_id, runs
  // `actor::BuildToolRequest`, dispatches to
  // `actor::ActorKeyedService::PerformActions(...)` and binds the
  // two-stage completion chain: `OnActorPerformActionsDone` →
  // `BuildActionsResultWithObservations` → `OnActionsResultBuilt`,
  // which walks the result proto and resolves `done` with the
  // SimpleObservation JSON.
  void RunActorAction(optimization_guide::proto::Actions actions,
                      std::string_view action_name,
                      ToolCallback done);
  ToolCallback PausableBudgetWrap(ToolCallback inner,
                                  base::TimeDelta budget,
                                  std::string action_name);
  scoped_refptr<PausableTimeout> RegisterPausableTimeout(
      base::TimeDelta budget,
      std::string name,
      base::OnceClosure on_timeout);
  void OnActorTaskStateChanged(actor::TaskId task_id,
                               actor::ActorTask::State state);
  void PauseActiveTimeouts();
  void ResumeActiveTimeouts();
  void PruneSettledTimeouts();
  void StopCurrentActorTaskForTimeout(std::string_view reason);

  // First completion stage: PerformActions returns. We immediately
  // hand off to `BuildActionsResultWithObservations` to attach
  // post-action page observations (so the LLM can see what changed).
  void OnActorPerformActionsDone(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      actor::mojom::ActionResultCode result_code,
      std::optional<size_t> index_of_failed_action,
      std::vector<actor::ActionResultWithLatencyInfo> action_results);

  // Two-stage `create_tab` flow.
  //
  // The actor proto can't carry "create tab + navigate" in one
  // `Actions` list because `NavigateAction` requires a non-null
  // `tab_id` at proto-validation time, but the new tab's id isn't
  // known until the `CreateTabAction` settles. So we run the create
  // first via a direct `PerformActions` call, then in
  // `OnCreateTabActionDone` diff the task's known-tabs set against
  // the pre-create snapshot to discover the new TabHandle, and
  // finally issue the navigate as a second `RunActorAction` so its
  // observation flows through the standard element-registry refresh.
  void OnCreateTabActionDone(
      ToolCallback done,
      std::string url,
      actor::ActorTask::TabHandleSet tabs_before,
      base::TimeTicks start_time,
      actor::mojom::ActionResultCode result_code,
      std::optional<size_t> index_of_failed_action,
      std::vector<actor::ActionResultWithLatencyInfo> action_results);

  // Observation-only path used by `screenshot` / `read_page` /
  // `find_text`.
  //
  // No proto Actions are dispatched. We mark the target tab as
  // "observe-once" via `ActorTask::ObserveTabOnce` and then call
  // `BuildActionsResultWithObservations` directly with an empty
  // `action_results` vector — the framework still walks the tab's
  // last-acted set and fetches APC + screenshot for each entry, so
  // marking a tab observed-once is enough to pull a fresh page snapshot
  // without ever running an action.
  //
  // `completion` selects the per-tool callback (standard
  // `OnActionsResultBuilt`, custom text extractor, custom find-text
  // walker, …). All variants resolve `done` once the result_proto is
  // in hand.
  using ObservationCompletion = base::OnceCallback<void(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      actor::mojom::ActionResultCode result_code,
      std::optional<size_t> index_of_failed_action,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::unique_ptr<optimization_guide::proto::ActionsResult> result_proto,
      std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> journal)>;

  void RunTabObservation(int32_t tab_id_value,
                         std::string_view action_name,
                         ToolCallback done,
                         ObservationCompletion completion);

  // Custom completion for `read_page`. Walks the
  // ActionsResult's `tab_observations[].annotated_page_content` tree
  // and accumulates leaf text up to `max_chars` characters. Resolves
  // `done` with `{ok, action, page_text, tab_id, url, title,
  // truncated}`.
  void OnReadPageObservationBuilt(
      std::string scope,
      int max_chars,
      int32_t requested_tab_id,
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      actor::mojom::ActionResultCode result_code,
      std::optional<size_t> index_of_failed_action,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::unique_ptr<optimization_guide::proto::ActionsResult> result_proto,
      std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> journal);

  // Custom completion for `find_text`. Walks the APC tree,
  // matches nodes whose visible text contains `query` (case-
  // insensitive), and returns a `matches[]` list with each match's
  // text plus its nearest clickable ancestor (so the LLM can pass
  // that ancestor's index directly to `click`).
  void OnFindTextObservationBuilt(
      std::string query,
      int max_results,
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      actor::mojom::ActionResultCode result_code,
      std::optional<size_t> index_of_failed_action,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::unique_ptr<optimization_guide::proto::ActionsResult> result_proto,
      std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> journal);

  // Second completion stage: the ActionsResult proto (with
  // observations attached) is ready. We walk it, populate
  // `node_registry_`, build a SimpleObservation-shaped JSON dict,
  // and resolve `done`.
  void OnActionsResultBuilt(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      actor::mojom::ActionResultCode result_code,
      std::optional<size_t> index_of_failed_action,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::unique_ptr<optimization_guide::proto::ActionsResult> result_proto,
      std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> journal);

  // Recursively walks one ContentNode tree. For every node that has
  // an interaction info / actionable role, registers it in
  // `node_registry_` keyed by `(content_node_id, tab_id)` and
  // appends a JSON representation (including the LLM-facing
  // `index`) to `out_elements`.
  void WalkContentNode(
      const optimization_guide::proto::ContentNode& node,
      const std::string& document_id,
      int tab_id,
      base::ListValue& out_elements);

  // Builds the SimpleObservation JSON returned to the LLM. Per tab:
  // tab_id / url / title / elements[] / has_screenshot. Non-fatal
  // observation errors (e.g. timeout) surface as flags so the LLM
  // can still reason about the action's success.
  base::DictValue BuildObservationDict(
      const optimization_guide::proto::ActionsResult& result_proto);

  // Phase 2 stop-gap for tools that aren't actor-wired yet.
  void RejectNotYetImplemented(std::string_view tool_name, ToolCallback done);

  // Looks up `index` in the per-session registry. When `tab_id` is
  // provided, resolution is unambiguous via `(index, tab_id)`;
  // otherwise the registry returns the most-recently-registered tab
  // for that index (sufficient for single-active-tab flows). On hit
  // returns the entry. On miss, rejects `done` with `index_not_found`
  // and returns nullptr — the caller MUST bail out (don't read or
  // move `done` after a null return).
  const NodeRegistry::Entry* LookupNode(int node_id,
                                        std::optional<int> tab_id,
                                        ToolCallback& done,
                                        std::string_view tool_name);

  std::optional<actor::TaskId> task_id_;

  // Per-action wall-clock budget. Default matches Browser-Use
  // (180s); JS can override via `browser.setAgentLoopSettings`,
  // which calls `ToolSession::set_action_timeout` → propagated here.
  base::TimeDelta action_timeout_ = base::Seconds(180);
  base::TimeDelta step_timeout_ = base::Seconds(180);
  bool actor_task_paused_ = false;
  base::CallbackListSubscription task_state_subscription_;
  std::vector<scoped_refptr<PausableTimeout>> active_timeouts_;

  // Maps `(index, tab_id)` (the LLM-facing identifier pair) → the
  // actor proto's `(document_identifier, tab_id)` plus diagnostic
  // metadata. Populated by `BuildObservationDict` / `WalkContentNode`
  // walking the ActionsResult observation tree after every action.
  // Accumulates ACROSS observations (not cleared per-tool) so the LLM
  // can carry intent across turns by index; only cleared on
  // `BrowserTool::OnStop`.
  NodeRegistry node_registry_;

  // Last-observation snapshot. Populated by `BuildObservationDict`
  // each time it walks an ActionsResult; read by ToolsAdapter when
  // wrapping the tool result with `agent_meta`. Plain string members
  // (no smart pointers) so the cost of always populating them is one
  // string copy per observation.
  std::string last_observation_url_;
  // Mirrors `last_observation_url_` for the primary tab's document title.
  // Used as a fallback when `OnBrowserActionItemSuccess` builds a minimal
  // observation for url_changed branches where ExtractWorkedTabObservation
  // returned a tab dict without `title` (e.g. APC not yet generated for
  // the post-navigation page).
  std::string last_observation_title_;
  int last_observation_interactive_count_ = 0;
  std::optional<int> last_observation_tab_id_;
  std::string last_observation_normalized_projection_;
  bool last_observation_focus_target_present_ = false;
  std::string last_observation_focus_target_id_;

  base::WeakPtrFactory<BrowserTool> weak_ptr_factory_{this};
};

}  // namespace nexus

// Declare the macro DSL's category accessor for `BrowserTool`. The
// matching `NEXUS_DEFINE_CATEGORY(BrowserTool, browser_tool)` lives
// in `browser_tool.cc`, which can include `tool_session.h` (the
// header can't, because `tool_session.h` already includes this file
// to embed `BrowserTool` as a member).
NEXUS_DECLARE_CATEGORY(nexus::BrowserTool);

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_H_
