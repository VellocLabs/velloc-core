#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

#include "base/callback_list.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
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
#include "components/page_content_annotations/content/page_context_fetcher.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tabs/public/tab_interface.h"
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

  // Notification override — stops the per-session actor task and
  // clears the node registry. Called by the cascade through
  // `ToolSession::OnStop(reason)`. Idempotent: safe to call more than
  // once.
  //
  // Note: this does NOT close the session's tabs. Tab teardown is
  // attached to `~BrowserTool` instead — the "tabs close with the
  // panel" UX maps onto the destructor running whenever ToolsAdapter
  // erases the session (kSessionDeleted) or the adapter itself dies
  // (panel close → port-service teardown).
  void OnStop(OnStopReason reason) override;

  // Tab bookkeeping API. Used by `RunCreateTabAction` to bind a
  // freshly-created tab to this session and by
  // `GetSessionScopedActiveTabHandleId` / `ResolveTabIdOrReject` (free
  // helpers in browser_tool_internal.h) to enumerate session-owned
  // tabs and read the last-worked-on tab.
  //
  // Inlined from the pre-refactor profile-scoped
  // `NexusSessionTabGroupRegistry`. The registry's purpose — bridge
  // BrowserTool lifetimes across per-turn OnStop / re-create cycles —
  // disappeared once ToolsAdapter stopped erasing on kCompleted /
  // kUserStopped; the state now belongs to BrowserTool itself.

  // Attach the session tag to the WebContents and add the tab to this
  // session's tab group (creating the group lazily on the first
  // session tab). Records the handle in `session_tabs_` for shutdown
  // teardown. No-op if the feature flag is off or the session key is
  // empty.
  //
  // Does NOT touch `last_work_tab_` — creating a tab isn't the same
  // as the LLM having worked on it. `RecordWorkTab` is called from
  // `RunActorAction` / `RunTabObservation` once an actual tool action
  // dispatches against the tab.
  void BindTabToSession(tabs::TabHandle handle);

  // Walks every Browser for this session's profile and returns the
  // handles of every tab whose NexusSessionTabTag matches this
  // session's key. The tag (not `session_tabs_`) is authoritative —
  // a tab the user manually dragged out of the group is still
  // session-owned. The flat-set member is a fast-path index for
  // common operations and is reconciled with the tag on shutdown.
  std::vector<tabs::TabHandle> CollectSessionTabs() const;

  // Returns the most-recent tab that a tool actually operated on for
  // this session, IF the handle is still alive AND still carries this
  // session's tag. Otherwise nullopt.
  //
  // Used by `GetSessionScopedActiveTabHandleId` as a fallback after
  // Chrome's active tab — answers "the LLM was last working on tab X;
  // it's calling a tab-less tool, so X is the natural default" when
  // the user's foreground tab isn't session-owned. Rename from the
  // earlier `last_active_tab_` clarifies the semantic: this records
  // *work*, not creation or activation.
  std::optional<tabs::TabHandle> LastWorkTab() const;

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
  // BrowserDownload. Two modes (exactly one of index|url): `index` clicks
  // an element to trigger a download and captures whatever the click
  // produces (handles blob:/POST/Content-Disposition/JS downloads with no
  // obtainable URL); `url` downloads a direct URL using the active tab's
  // session. Saves into `<cwd>/downloads/` and resolves via DownloadCapture
  // (bounded wait + in-progress reporting for large files). See
  // download_capture.{h,cc}.
  void RunDownloadOnUI(::nexus::generated::DownloadInput input,
                   ToolCallback done) override;
  // BrowserUpload. Arms `NexusPendingUpload` for the target tab with the
  // resolved local file path, then clicks the file <input> via the actor;
  // the chooser the click opens is answered by the guarded hook in
  // Browser::RunFileChooser. The model gets the post-click observation
  // (which should show the attached filename). See
  // upload/nexus_file_chooser_interceptor.{h,cc}.
  void RunUploadOnUI(::nexus::generated::UploadInput input,
                   ToolCallback done) override;

  // BrowserEvaluate. Runs arbitrary JavaScript in the active tab's MAIN world
  // via a one-shot CDP `Runtime.evaluate` (BrowserEvaluateSession in
  // browser_evaluate.{h,cc}; awaitPromise=true, returnByValue=true). Always
  // available — a first-class browser tool like the rest of the Browser*
  // surface (the former `nexus.browser_tools_allow_evaluate` opt-in gate was
  // removed). Returns the JSON-stringified result (capped at 20k chars) or,
  // when the JS threw, the CDP exceptionDetails surfaced as `{ error: …,
  // exception: … }`.
  void RunEvaluateOnUI(::nexus::generated::EvaluateInput input,
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
  //
  // `working_tab_id` is the tab this call is operating on (explicit
  // input arg or resolved via `ResolveTabIdOrReject`). Used solely to
  // call `RecordWorkTab(*working_tab_id)` before dispatch so the
  // session's `last_work_tab_` advances to track the LLM's intent —
  // the observation builder downstream reads `last_work_tab_`
  // directly and doesn't need the value plumbed through the callback
  // chain. `std::nullopt` means "no specific working tab" (e.g.
  // `close_tab`); the recorder skips the update in that case.
  void RunActorAction(optimization_guide::proto::Actions actions,
                      std::string_view action_name,
                      ToolCallback done,
                      std::optional<int32_t> working_tab_id);
  ToolCallback PausableBudgetWrap(ToolCallback inner,
                                  base::TimeDelta budget,
                                  std::string action_name);
  scoped_refptr<browser_tool_internal::PausableTimeout> RegisterPausableTimeout(
      base::TimeDelta budget,
      std::string name,
      base::OnceClosure on_timeout);
  // custom-browser: Chromium 149's TaskStateChangedCallback passes ActorTask&.
  void OnActorTaskStateChanged(actor::ActorTask& task);
  void PauseActiveTimeouts();
  void ResumeActiveTimeouts();
  void PruneSettledTimeouts();
  void StopCurrentActorTaskForTimeout(std::string_view reason);

  // First completion stage: PerformActions returns. We immediately
  // hand off to `BuildActionsResultWithObservations` to attach
  // post-action page observations (so the LLM can see what changed).
  // custom-browser: Chromium 149's PerformActionsCallback passes only
  // action_results (result_code/index are folded in and derived in the body).
  //
  // `actions_for_retry` carries a copy of the dispatched Actions proto and
  // `retried` whether the one-shot offscreen retry has already run: a
  // freshly-created same-window tab can be transiently 0-sized while the
  // panel's agent zone lays out, making the actor's visibility hit-test
  // report `kElementOffscreen` for an element that is about to become
  // perfectly interactable. One delayed retry absorbs that race; a
  // genuinely offscreen element still surfaces the error (after the retry).
  void OnActorPerformActionsDone(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      optimization_guide::proto::Actions actions_for_retry,
      bool retried,
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
      std::vector<actor::ActionResultWithLatencyInfo> action_results);

  // Reply for RunDownloadOnUI's url-mode mkdir hop. The forced-path download
  // writer won't create parent directories, and a freshly-created session
  // workspace has no `downloads/` folder yet — so we create `dest_dir` on a
  // blocking pool first, then resume here (back on the UI thread) to
  // re-resolve the source tab + download manager and kick off the transfer.
  // `dir_ok` is the CreateDirectory result. `url` is passed as a string so
  // this header needn't depend on url/gurl.h.
  void StartUrlDownloadAfterDirReady(int32_t tab_id,
                                     std::string url,
                                     base::FilePath dest_dir,
                                     std::string name,
                                     base::TimeDelta budget,
                                     ToolCallback done,
                                     bool dir_ok);

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
  // custom-browser: matches Chromium 149's BuildActionsResultWithObservations
  // callback (result_code/index folded into action_results; screenshot
  // collection options added before the result proto).
  using ObservationCompletion = base::OnceCallback<void(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::optional<page_content_annotations::ScreenshotOptions::
                        ScreenshotCollectionOptions>
          screenshot_collection_options,
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
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::optional<page_content_annotations::ScreenshotOptions::
                        ScreenshotCollectionOptions>
          screenshot_collection_options,
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
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::optional<page_content_annotations::ScreenshotOptions::
                        ScreenshotCollectionOptions>
          screenshot_collection_options,
      std::unique_ptr<optimization_guide::proto::ActionsResult> result_proto,
      std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> journal);

  // Second completion stage: the ActionsResult proto (with
  // observations attached) is ready. We walk it, populate
  // `node_registry_`, build a SimpleObservation-shaped JSON dict,
  // and resolve `done`. Reads `LastWorkTab()` to decide which tab
  // gets the full element walk; every other session-owned tab in
  // the observation gets basics only.
  // custom-browser: matches Chromium 149's BuildActionsResultWithObservations
  // callback (result_code/index folded into action_results; screenshot options
  // added).
  void OnActionsResultBuilt(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      std::vector<actor::ActionResultWithLatencyInfo> action_results,
      actor::TaskId task_id,
      bool skip_async_observation_information,
      std::optional<page_content_annotations::ScreenshotOptions::
                        ScreenshotCollectionOptions>
          screenshot_collection_options,
      std::unique_ptr<optimization_guide::proto::ActionsResult> result_proto,
      std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> journal);

  // Late-navigation settle guard (see the block comment at its call site in
  // `OnActionsResultBuilt`): the just-built observation raced a navigation
  // on the work tab, so poll (150ms × `polls_left`) until the raced
  // navigation COMMITS (committed URL moves off `entry_committed_url`) or
  // the tab is quiescent — not loading, no pending entry — then take ONE
  // fresh observation via `RunTabObservation` and resolve `done` with it.
  // If the work tab dies while waiting, resolves with `fallback_result`
  // (the observation that was already built) instead — the action itself
  // succeeded and must not be failed retroactively.
  // `entry_committed_url` is a GURL spec, carried as a string so this
  // header needn't depend on url/gurl.h (same convention as
  // `StartUrlDownloadAfterDirReady`).
  void WaitForWorkTabQuiescenceThenReobserve(ToolCallback done,
                                             std::string action_name,
                                             base::DictValue fallback_result,
                                             std::string entry_committed_url,
                                             int polls_left);

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

  // Pre-pass over the APC tree that fills `observation_label_targets_`
  // from every `<label for=...>` node (APC's `label_for_dom_node_id`).
  // Run once per working-tab walk in `BuildObservationDict`, before
  // `WalkContentNode`, so the walk can retarget hidden checkboxes/radios
  // to their visible label. See `observation_label_targets_`.
  void CollectLabelTargets(
      const optimization_guide::proto::ContentNode& node);

  // Builds the SimpleObservation JSON returned to the LLM.
  //
  // The **working tab** is read directly from `LastWorkTab()` —
  // the session's stored "tab the most recent action ran against".
  // It was updated by `RecordWorkTab` at the top of `RunActorAction`
  // / `RunTabObservation` before dispatch, so by the time this
  // builds the observation it points at the just-acted-upon tab.
  //
  // For the working tab in `result_proto.tabs()`: emits the full
  // payload — `tab_id` / `url` / `title` / `elements[]` (and, on the
  // BrowserScreenshot path, `screenshot`) — walks the APC tree via
  // `WalkContentNode`, populates `node_registry_`, and updates the
  // `last_observation_*` snapshot used by `agent_meta`.
  //
  // For every other session-owned tab in the observation: emits
  // basics only — `tab_id` / `url` / `title`. No `elements[]`,
  // no screenshot, no registry / projection updates. The LLM
  // sees that those tabs exist but must `BrowserActivateTab(id)`
  // to bring one into focus and see its interactive elements.
  //
  // If `LastWorkTab()` returns nullopt (no work tab recorded yet,
  // or the recorded tab has died — e.g. just-closed by `close_tab`),
  // every entry is basics-only.
  //
  // Cross-session tab filter (`kSessionScopedTabs`) still applies
  // here — tabs whose `NexusSessionTabTag` doesn't match this
  // session are dropped entirely.
  // `include_screenshot_image`: when true AND the working tab carries a
  // non-empty screenshot blob, the working tab's dict gets a
  // `screenshot: { mime, data_base64 }` field (base64 of the captured
  // bytes) so the JS side can turn it into an image content block for
  // the model. Only `BrowserScreenshot` sets this — every other tool
  // omits the image entirely (no flag) to avoid bloating its result with
  // pixels the model didn't ask for. The `screenshot` key's presence is
  // the sole, unambiguous "image included" signal.
  base::DictValue BuildObservationDict(
      const optimization_guide::proto::ActionsResult& result_proto,
      bool include_screenshot_image = false);

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

  // Close every tagged tab and clear the bookkeeping. Idempotent.
  // Called exclusively from `~BrowserTool` — see the comment above
  // `OnStop` for why teardown is destructor-driven rather than
  // OnStop-driven.
  void CloseAllSessionTabsOnDestroy();

  std::optional<actor::TaskId> task_id_;

  // Records `handle` as the tab a tool just operated on. Called from
  // `RunActorAction` / `RunTabObservation` when their resolved
  // working_tab_id is non-null. Validates the handle is session-owned
  // before storing — a no-op on cross-session leaks. The "natural
  // default" for the next tab-less call.
  void RecordWorkTab(tabs::TabHandle handle);

  // Session-owned tab bookkeeping (formerly the per-profile
  // `NexusSessionTabGroupRegistry`'s `SessionState` row).
  //
  // `tab_group_id_` is std::nullopt until the first `BindTabToSession`
  // creates the visual group. The group dies automatically once the
  // last tab in it closes (Chromium auto-deletes empty groups).
  //
  // `session_tabs_` is the fast-path membership index. Reconciled with
  // the authoritative `NexusSessionTabTag` on shutdown via
  // `CollectSessionTabs()` so a tab the user dragged out of the group
  // still gets closed on session teardown.
  //
  // `last_work_tab_` is the most-recent tab a tool DISPATCHED an
  // action against. Set via `RecordWorkTab` in `RunActorAction` /
  // `RunTabObservation`, NOT in `BindTabToSession` /
  // `RunActivateTabOnUI` — creating or focus-cycling a tab isn't the
  // same as the LLM working on it. `LastWorkTab()` reads it back,
  // gating on the handle still being alive and still session-tagged.
  // Used by `GetSessionScopedActiveTabHandleId` as a fallback after
  // Chrome's foreground tab when that one isn't session-owned.
  std::optional<tab_groups::TabGroupId> tab_group_id_;
  base::flat_set<tabs::TabHandle> session_tabs_;
  tabs::TabHandle last_work_tab_;

  // Per-session-tab capture hold. Agent tabs live in a separate browsing
  // window that comes up behind the kNexus chat window; Chromium's
  // occlusion/background throttling would otherwise freeze that window's
  // renderer so its tabs never paint (empty observations, read_page /
  // screenshot hang). A `WebContents::IncrementCapturerCount` hold keeps each
  // session tab's renderer producing compositor frames "regardless of their
  // backgrounded or on-screen occlusion state" (per web_contents.h). Scoped
  // strictly to session-owned agent tabs — the user's own tabs are never
  // held, so they keep default throttling. The release closure is weak-bound
  // to the WebContents, so it's a safe no-op if the tab closed first; the map
  // is destroyed with the BrowserTool, dropping every hold.
  base::flat_map<tabs::TabHandle, base::ScopedClosureRunner>
      session_tab_capture_holds_;

  // Per-action wall-clock budget. Default matches Browser-Use
  // (180s); JS can override via `browser.setAgentLoopSettings`,
  // which calls `ToolSession::set_action_timeout` → propagated here.
  base::TimeDelta action_timeout_ = base::Seconds(180);
  base::TimeDelta step_timeout_ = base::Seconds(180);
  bool actor_task_paused_ = false;
  base::CallbackListSubscription task_state_subscription_;
  std::vector<scoped_refptr<browser_tool_internal::PausableTimeout>>
      active_timeouts_;

  // A visible <label> that drives a form control, resolved from APC's
  // `label_for_dom_node_id`. `node_id` is the label's own DOM
  // content_node_id; `box` is its `[x, y, w, h]` viewport rect; `text` is the
  // label's visible text (e.g. "Honda") — surfaced as the control's `text`
  // when the control carries no accessible name of its own, so the model can
  // target "the checkbox labeled Honda" instead of guessing its position in
  // an unlabeled list.
  struct LabelTarget {
    int node_id = 0;
    std::array<int, 4> box{};
    std::string text;
  };
  // Per-observation index: hidden form control's node_id → its visible
  // label. Built by a pre-pass over the APC tree in `BuildObservationDict`
  // (before `WalkContentNode`), then read during the walk so a checkbox /
  // radio whose own rect is empty (custom "sr-only" input behind a styled
  // <label for=...>) is RE-TARGETED to the label: clicking the label
  // toggles the input via native HTML, so the LLM gets a working,
  // box-bearing element instead of an un-clickable offscreen one. Cleared
  // and repopulated per working-tab walk; never persisted across actions.
  base::flat_map<int, LabelTarget> observation_label_targets_;

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

  // One-shot latch for the late-navigation observation retry. Set when
  // `OnActionsResultBuilt` hands off to
  // `WaitForWorkTabQuiescenceThenReobserve`; read-and-cleared at the top of
  // the next `OnActionsResultBuilt` entry so the retried observation skips
  // the guard — at most ONE re-observation per action, never a loop. One
  // action is in flight per session at a time (the agent loop serializes
  // tool calls), which is what makes a plain member a safe carrier.
  bool nav_settle_observation_retried_ = false;

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
