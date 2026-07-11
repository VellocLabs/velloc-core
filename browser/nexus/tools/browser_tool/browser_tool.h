#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_

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
#include "custom_browser/browser/nexus/tools/browser_tool/node_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_macros.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

// Generated category-aggregate base. One *.gen.h per category holds
// the abstract base class (`BrowserToolBase` here) whose virtuals
// aggregate every tool declared under
// declarations/browser_tool/<name>.tool.json. The category header
// transitively pulls in each per-tool *.gen.h (for the typed
// `<Name>Input` structs the virtuals receive), so a single include
// covers the entire generated-base-class surface.
#include "custom_browser/browser/nexus/tools/browser_tool.gen.h"

class Profile;

namespace nexus {

namespace browser_tool_internal {
class PausableTimeout;  // defined in browser_tool_internal.h
}  // namespace browser_tool_internal

class ToolSession;  // for CategoryAccessor specialization below

// Per-LLM-session class that owns the browser-actor task and hosts every
// `browser_*` tool method. One instance per session, constructed as a
// member of `ToolSession`.
//
// The `task_id_` member is per-instance: on the first actor-driven
// tool call, `EnsureTaskCreated` mints an `actor::TaskId` via
// `actor::ActorKeyedService` and stashes it here. `OnStop(reason)`
// stops the task and resets `task_id_` so the cleanup is idempotent.
//
// Inherits `generated::BrowserToolBase` so the codegen-emitted
// dispatcher in nexus_tools_registry.gen.cc can route via
// `category->Run<Name>(input, done)` for every tool declared under
// declarations/browser_tool/. The overrides delegate to the existing
// macro-based action handlers (`RunScreenshotAction` & friends) so no
// behavior changes during Phase 3 migration. As more tools migrate,
// new pure virtuals appear automatically on `BrowserToolBase` (one
// per `*.tool.json` in the category) and BrowserTool grows matching
// overrides. Phase 4 removes the old macro entries once every action
// has migrated.
class BrowserTool : public ToolBase,
                    public ::nexus::generated::BrowserToolBase {
 public:
  BrowserTool(ToolSession* session, Profile* profile,
              const std::string& session_key);
  ~BrowserTool() override;

  BrowserTool(const BrowserTool&) = delete;
  BrowserTool& operator=(const BrowserTool&) = delete;

  // Required by the framework's catalog construction (called by
  // `ToolCatalog::Get()`). Phase 4 retired the macro-based tool
  // registration this method used to populate; the body is now a
  // no-op kept around as a migration anchor in case a future
  // category needs the macro DSL again. The 24 split browser tools
  // are registered via `generated::PopulateRegistry()` instead, see
  // tools/codegen/generate.py + declarations/browser_tool/.
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

  // Overrides of generated::BrowserToolBase virtuals. The codegen-
  // emitted dispatcher routes here with typed input structs (from each
  // tool's *.gen.h) after extracting / validating args. Each body
  // delegates to the existing `*Action` helper so the underlying
  // logic stays in one place during the macro→declarative migration.
  // Phase 4 inlines the *Action helpers once each is the sole caller.
  //
  // Public visibility is required because the codegen dispatcher in
  // nexus_tools_registry.gen.cc invokes through a `BrowserTool*` it
  // gets from `CategoryAccessor::Get(session)`.
  void RunScreenshotOnUI(::nexus::generated::ScreenshotInput input,
                     ToolCallback done) override;
  void RunReadPageOnUI(::nexus::generated::ReadPageInput input,
                   ToolCallback done) override;
  void RunNavigateOnUI(::nexus::generated::NavigateInput input,
                   ToolCallback done) override;
  void RunBackOnUI(::nexus::generated::BackInput input,
               ToolCallback done) override;
  void RunCloseTabOnUI(::nexus::generated::CloseTabInput input,
                   ToolCallback done) override;
  void RunActivateTabOnUI(::nexus::generated::ActivateTabInput input,
                      ToolCallback done) override;
  void RunClickOnUI(::nexus::generated::ClickInput input,
                ToolCallback done) override;
  void RunInputOnUI(::nexus::generated::InputInput input,
                ToolCallback done) override;
  void RunSelectOnUI(::nexus::generated::SelectInput input,
                 ToolCallback done) override;
  void RunHoverOnUI(::nexus::generated::HoverInput input,
                ToolCallback done) override;
  void RunDragAndDropOnUI(::nexus::generated::DragAndDropInput input,
                      ToolCallback done) override;
  void RunScrollToOnUI(::nexus::generated::ScrollToInput input,
                   ToolCallback done) override;
  void RunScrollOnUI(::nexus::generated::ScrollInput input,
                 ToolCallback done) override;
  void RunSearchOnUI(::nexus::generated::SearchInput input,
                 ToolCallback done) override;
  void RunDoneOnUI(::nexus::generated::DoneInput input,
               ToolCallback done) override;
  void RunMediaControlOnUI(::nexus::generated::MediaControlInput input,
                       ToolCallback done) override;
  void RunMediaStateOnUI(::nexus::generated::MediaStateInput input,
                     ToolCallback done) override;
  void RunReadElementOnUI(::nexus::generated::ReadElementInput input,
                      ToolCallback done) override;
  void RunFindTextOnUI(::nexus::generated::FindTextInput input,
                   ToolCallback done) override;
  // Phase 13: batch form-fill. Runs the entries in `fields[]`
  // sequentially on the active tab via the same primitives used by
  // RunInputOnUI / RunSelectOnUI / RunClickOnUI. Optionally clicks
  // `submit_index` at the end. `fields` is declared as array<object>
  // in the schema (untyped at codegen level — pragmatic Option B from
  // the planning conversation), so the body manually parses each
  // base::DictValue into a typed FieldOp inside browser_tool.cc.
  void RunFillFormOnUI(::nexus::generated::FillFormInput input,
                   ToolCallback done) override;

 private:
  // ----------------------------------------------------------------
  // Internal helpers. Most tool bodies live inline in the public
  // `Run<Name>` overrides above; this section holds only helpers
  // shared across overrides.
  // ----------------------------------------------------------------

  // Two callers: `RunNavigateOnUI` (when input.new_tab=true) and
  // `RunSearch` (which builds a search-engine URL then opens it in a
  // new tab via this helper). The two-stage CreateTab + Navigate flow
  // lives here too because the new tab's `tab_id` isn't known until
  // the first stage settles — see `OnCreateTabActionDone`.
  //
  // Phase 13: the `create_tab` tool was retired and folded into
  // `BrowserNavigate { url, new_tab }`. The legacy `reuse_visible_tab`
  // optimization (reuse the active tab if it's already on about:blank)
  // is now a C++-side automatic decision inside this helper rather
  // than a model-facing knob — see body for details.
  //
  // Phase 13.1: `action_name` is plumbed through so the result envelope's
  // `action` field matches the user-facing tool (e.g. "search" /
  // "navigate") instead of leaking the internal "create_tab" stage name.
  // Without this, callers of `BrowserSearch { new_tab: true }` saw
  // `action: "create_tab"` in the result and would mis-interpret it as
  // "no search happened, just a tab opened" and retry.
  void RunCreateTabAction(std::string url,
                          std::string action_name,
                          ToolCallback done);

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
  scoped_refptr<browser_tool_internal::PausableTimeout> RegisterPausableTimeout(
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
      std::string action_name,
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
  std::vector<scoped_refptr<browser_tool_internal::PausableTimeout>>
      active_timeouts_;

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
  // Mirrors `last_observation_url_` for the primary tab's document
  // title. Populated by `BuildObservationDict`; read by adapters (and
  // by url_changed bookkeeping inside individual tool helpers) so a
  // url_changed transition that lands before APC has regenerated
  // still has a non-empty title in the result envelope.
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

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_
