#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

#include "base/callback_list.h"
#include "base/containers/span.h"
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
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/node_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

// Generated category-aggregate base. One *.gen.h per category holds
// the abstract base class (`BrowserToolBase` here) whose virtuals
// aggregate every tool declared under
// declarations/browser_tool/<name>.tool.json. The category header
// transitively pulls in each per-tool *.gen.h (for the typed
// `<Name>Input` structs the virtuals receive), so a single include
// covers the entire generated-base-class surface.
#include "custom_browser/browser/nexus/api/browser/browser_api.gen.h"

class Profile;

namespace nexus {

namespace browser_tool_internal {
class PausableTimeout;  // defined in browser_tool_internal.h
}  // namespace browser_tool_internal

class ToolSession;  // for CategoryAccessor specialization below

// Per-LLM-session class owning the browser-actor task and hosting every
// `browser_*` tool, a member of `ToolSession`. `EnsureTaskCreated` mints
// `task_id_` on the first actor call; `OnStop` stops and resets it.
class BrowserTool : public ToolBase {
 public:
  BrowserTool(ToolSession* session, Profile* profile,
              const std::string& session_key);
  ~BrowserTool() override;

  BrowserTool(const BrowserTool&) = delete;
  BrowserTool& operator=(const BrowserTool&) = delete;


  // Stops the per-session actor task, clears the node registry and drops the
  // turn's capture holds. Idempotent.
  // Does not close tabs: `~BrowserTool` does, when the session is erased or the
  // adapter dies.
  void OnStop(OnStopReason reason) override;

  // Tab bookkeeping API: binds created tabs to this session and lets the
  // browser_tool_internal.h helpers enumerate session tabs and the work tab.

  // Tags the WebContents and adds the tab to this session's (lazily created)
  // tab group and `session_tabs_`. No-op if the flag is off or the key is empty.
  // Does not touch `last_work_tab_`.
  void BindTabToSession(tabs::TabHandle handle);

  // Walks every Browser for this session's profile and returns the
  // handles of every tab whose NexusSessionTabTag matches this
  // session's key. The tag (not `session_tabs_`) is authoritative —
  // a tab the user manually dragged out of the group is still
  // session-owned. The flat-set member is a fast-path index for
  // common operations and is reconciled with the tag on shutdown.
  std::vector<tabs::TabHandle> CollectSessionTabs() const;

  // The most recent tab a tool operated on, if still alive and session-tagged;
  // the fallback after Chrome's active tab for tab-less tools.
  std::optional<tabs::TabHandle> LastWorkTab() const;

  // Holds every session tab visible (a capture hold) until `OnStop`, so an
  // agent tab behind the chat window keeps painting while the turn's tools
  // run. Called before each browser tool dispatch. Idempotent.
  void HoldSessionTabsForTurn();

  // Holds `handles` until `OnStop`; skips tabs already held or gone.
  void HoldTabsForTurn(base::span<const tabs::TabHandle> handles);

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

  // Last-observation snapshot read by ToolSessionRegistry for `agent_meta`.
  // `last_normalized_projection_` is a deterministic serialization of the
  // element payload without tab/document ids; its SHA-256 prefix is the dom_hash.
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

  // Observation short-circuit control: `SetObservationDiffEnabled` is the guest
  // opt-in (off: always full); `MarkObservationStale` forces the next one full
  // after compaction / restore / replay.
  void SetObservationDiffEnabled(bool enabled) {
    observation_diff_enabled_ = enabled;
  }
  void MarkObservationStale() { observation_baseline_.MarkStale(); }
  // Focus target may be absent (no focused element); the bool flag
  // separates "no focus" from "focus present but with empty id".
  bool has_last_observation_focus_target() const {
    return last_observation_focus_target_present_;
  }
  const std::string& last_observation_focus_target_id() const {
    return last_observation_focus_target_id_;
  }

  // Overrides of generated::BrowserToolBase virtuals, routed with typed inputs by
  // the codegen dispatcher (hence public); each delegates to its `*Action`.
  void RunScreenshotOnUI(::nexus::generated::BrowserScreenshotInput input,
                     ToolCallback done);
  void RunReadPageOnUI(::nexus::generated::BrowserReadPageInput input,
                   ToolCallback done);
  void RunNavigateOnUI(::nexus::generated::BrowserNavigateInput input,
                   ToolCallback done);
  void RunBackOnUI(::nexus::generated::BrowserBackInput input,
               ToolCallback done);
  void RunCloseTabOnUI(::nexus::generated::BrowserCloseTabInput input,
                   ToolCallback done);
  void RunActivateTabOnUI(::nexus::generated::BrowserActivateTabInput input,
                      ToolCallback done);
  void RunClickOnUI(::nexus::generated::BrowserClickInput input,
                ToolCallback done);
  void RunInputOnUI(::nexus::generated::BrowserInputInput input,
                ToolCallback done);
  void RunSelectOnUI(::nexus::generated::BrowserSelectInput input,
                 ToolCallback done);
  void RunHoverOnUI(::nexus::generated::BrowserHoverInput input,
                ToolCallback done);
  void RunDragAndDropOnUI(::nexus::generated::BrowserDragAndDropInput input,
                      ToolCallback done);
  void RunScrollToOnUI(::nexus::generated::BrowserScrollToInput input,
                   ToolCallback done);
  void RunScrollOnUI(::nexus::generated::BrowserScrollInput input,
                 ToolCallback done);
  void RunSearchOnUI(::nexus::generated::BrowserSearchInput input,
                 ToolCallback done);
  void RunDoneOnUI(::nexus::generated::BrowserDoneInput input,
               ToolCallback done);
  void RunMediaControlOnUI(::nexus::generated::BrowserMediaControlInput input,
                       ToolCallback done);
  void RunMediaStateOnUI(::nexus::generated::BrowserMediaStateInput input,
                     ToolCallback done);
  void RunReadElementOnUI(::nexus::generated::BrowserReadElementInput input,
                      ToolCallback done);
  void RunFindTextOnUI(::nexus::generated::BrowserFindTextInput input,
                   ToolCallback done);
  // Phase 13: batch form-fill. Runs the entries in `fields[]`
  // sequentially on the active tab via the same primitives used by
  // RunInputOnUI / RunSelectOnUI / RunClickOnUI. Optionally clicks
  // `submit_index` at the end. `fields` is declared as array<object>
  // in the schema (untyped at codegen level — pragmatic Option B from
  // the planning conversation), so the body manually parses each
  // base::DictValue into a typed FieldOp inside browser_tool.cc.
  void RunFillFormOnUI(::nexus::generated::BrowserFillFormInput input,
                   ToolCallback done);
  // BrowserDownload. Two modes (exactly one of index|url): `index` clicks
  // an element to trigger a download and captures whatever the click
  // produces (handles blob:/POST/Content-Disposition/JS downloads with no
  // obtainable URL); `url` downloads a direct URL using the active tab's
  // session. Saves into `<cwd>/downloads/` and resolves via DownloadCapture
  // (bounded wait + in-progress reporting for large files). See
  // download_capture.{h,cc}.
  void RunDownloadOnUI(::nexus::generated::BrowserDownloadInput input,
                   ToolCallback done);
  // BrowserUpload. Arms `NexusPendingUpload` for the target tab with the
  // resolved local file path, then clicks the file <input> via the actor;
  // the chooser the click opens is answered by the guarded hook in
  // Browser::RunFileChooser. The model gets the post-click observation
  // (which should show the attached filename). See
  // upload/nexus_file_chooser_interceptor.{h,cc}.
  void RunUploadOnUI(::nexus::generated::BrowserUploadInput input,
                   ToolCallback done);

  // BrowserEvaluate. Runs arbitrary JavaScript in the active tab's MAIN world
  // via a one-shot CDP `Runtime.evaluate` (BrowserEvaluateSession in
  // browser_evaluate.{h,cc}; awaitPromise=true, returnByValue=true). Always
  // available — a first-class browser tool like the rest of the Browser*
  // surface (the former `nexus.browser_tools_allow_evaluate` opt-in gate was
  // removed). Returns the JSON-stringified result (capped at 20k chars) or,
  // when the JS threw, the CDP exceptionDetails surfaced as `{ error: …,
  // exception: … }`.
  void RunEvaluateOnUI(::nexus::generated::BrowserEvaluateInput input,
                       ToolCallback done);

 private:
  // ----------------------------------------------------------------
  // Internal helpers. Most tool bodies live inline in the public
  // `Run<Name>` overrides above; this section holds only helpers
  // shared across overrides.
  // ----------------------------------------------------------------

  // Opens `url` in a new tab for `RunNavigateOnUI` (new_tab) and `RunSearch`;
  // two-stage, since the tab id is unknown until creation settles.
  // `action_name` keeps the result's `action` the user-facing tool name.
  void RunCreateTabAction(std::string url,
                          std::string action_name,
                          ToolCallback done);

  // Lazily create the per-instance `actor::TaskId` on first
  // actor-driven tool call. Returns std::nullopt and rejects `done`
  // if the actor service / policy checker is unavailable.
  std::optional<actor::TaskId> EnsureTaskCreated(ToolCallback& done);

  // Dispatches `actions` via PerformActions and chains
  // `OnActorPerformActionsDone` → `BuildActionsResultWithObservations` →
  // `OnActionsResultBuilt`. `working_tab_id` (nullopt = none) is recorded via
  // `RecordWorkTab` before dispatch.
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

  // First completion stage: attach post-action observations. Chromium 149
  // passes only action_results. `actions_for_retry` / `retried` allow one
  // delayed retry of `kElementOffscreen` for a transiently 0-sized new tab.
  void OnActorPerformActionsDone(
      ToolCallback done,
      std::string action_name,
      base::TimeTicks start_time,
      optimization_guide::proto::Actions actions_for_retry,
      bool retried,
      std::vector<actor::ActionResultWithLatencyInfo> action_results);

  // Two-stage `create_tab` flow: NavigateAction needs a tab id, so the create
  // runs first, the new TabHandle is found by diffing known tabs, then the
  // navigate goes through `RunActorAction`.
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

  // Observation-only path (`screenshot` / `read_page` / `find_text`): marks the
  // tab via `ActorTask::ObserveTabOnce` and builds observations with no actions.
  // `completion` selects the per-tool callback; all resolve `done`.
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
      bool include_observation,
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
      bool include_observation,
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

  // Second completion stage: walk the result, populate `node_registry_`, build
  // the observation dict (full only for `LastWorkTab()`) and resolve `done`.
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

  // Late-navigation settle guard: poll (150ms × `polls_left`) until the raced
  // navigation commits or the tab is quiescent, then re-observe once. If the tab
  // dies, resolve with `fallback_result`. `entry_committed_url` is a GURL spec.
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

  // Builds the observation JSON. The working tab (`LastWorkTab()`) gets the full
  // payload (`elements[]`, registry and `last_observation_*` updates, plus
  // `screenshot: {mime, data_base64}` when `include_screenshot_image`); other
  // session tabs get basics; other sessions' tabs are dropped. `force_full`
  // never collapses to an `{unchanged}` stub. The walk only stages the
  // baseline: the caller must `Commit()` if elements ship, else `Discard()`.
  base::DictValue BuildObservationDict(
      const optimization_guide::proto::ActionsResult& result_proto,
      bool include_screenshot_image = false,
      bool force_full = false);

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

  // Session tab bookkeeping. `tab_group_id_` is unset until the first bind.
  // `session_tabs_` is reconciled with `NexusSessionTabTag` at shutdown so
  // dragged-out tabs still close. `last_work_tab_` is set only when a tool
  // dispatches against a tab.
  std::optional<tab_groups::TabGroupId> tab_group_id_;
  base::flat_set<tabs::TabHandle> session_tabs_;
  tabs::TabHandle last_work_tab_;

  // Takes the capture hold on `handle` if it has none and the tab is alive.
  void TakeCaptureHold(tabs::TabHandle handle);

  // Turn-scoped capture holds on agent tabs: taken by
  // `HoldSessionTabsForTurn` / `BindTabToSession`, dropped by `OnStop`.
  // Releases are weak-bound to the WebContents, so a closed tab's entry is a
  // safe no-op.
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
  // each time it walks an ActionsResult; read by ToolSessionRegistry when
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

  // Observation short-circuit state: `observation_baseline_` holds the
  // projection + document id the model last received in full;
  // `observation_diff_enabled_` gates it (default off).
  browser_tool_internal::ObservationBaseline observation_baseline_;
  bool observation_diff_enabled_ = false;

  // Per-tab document identifier last seen by the observation walk. When a
  // tab's document changes (navigation, cross-document load) every
  // content_node_id from the old document is dead, so `BuildObservationDict`
  // calls `node_registry_.ClearTab(tab_id)` before re-walking to evict the
  // stale entries — otherwise `Find(index)` could resolve a stale index to
  // a defunct document_id and the actor would reject the action. Cleared
  // alongside `node_registry_` in `OnStop`.
  base::flat_map<int, std::string> registry_document_by_tab_;

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


#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_H_
