#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/hash/hash.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "components/actor/core/task_source_info.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor_webui.mojom.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/session/nexus_session_features.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tracker.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/upload/nexus_pending_agent_notice.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

namespace nexus {

// File-local shorthand: `using` directive scoped to this TU only so we
// can call ResolveTabIdOrReject / MaybePrependActivateTab / etc.
// without qualifying every call site. Bad style in headers; acceptable
// in a single .cc file. Mirrored in browser_tool_actions.cc — both
// files share the same helper namespace and need the same alias.
using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

// All cross-cutting helpers (ActionParams, ToActorStoppedReason,
// GetActiveBrowserForProfile, GetActiveWindowSessionId,
// GetActiveTabHandleId, ResolveTabIdOrReject, MaybePrependActivateTab,
// GetPolicyChecker, ContentAttributeTypeName, ExtractNodeText,
// IsActionable, TruncateUtf8Snippet) plus the PausableTimeout +
// PausableToolCallbackHolder classes have been moved out to
// browser_tool_internal.{h,cc}. The `using namespace
// browser_tool_internal;` directive at the top of this TU lets us
// continue calling them unqualified from BrowserTool method bodies.

// Deterministic color picker for the session's tab group. Hashing the
// session_key onto one of eight non-grey palette entries gives every
// session a stable color across the browser-process lifetime without
// requiring any persisted state — group lifetime is in-process anyway.
tab_groups::TabGroupColorId DeterministicColorFor(
    std::string_view session_key) {
  static constexpr std::array<tab_groups::TabGroupColorId, 8> kPalette = {
      tab_groups::TabGroupColorId::kBlue,
      tab_groups::TabGroupColorId::kRed,
      tab_groups::TabGroupColorId::kYellow,
      tab_groups::TabGroupColorId::kGreen,
      tab_groups::TabGroupColorId::kPink,
      tab_groups::TabGroupColorId::kPurple,
      tab_groups::TabGroupColorId::kCyan,
      tab_groups::TabGroupColorId::kOrange,
  };
  const size_t i = base::FastHash(session_key) % kPalette.size();
  return kPalette[i];
}

std::u16string TabGroupTitleFor(std::string_view session_key) {
  // Twelve chars is enough to disambiguate typical short session ids
  // without bloating the tab-strip pill.
  constexpr size_t kMaxLen = 12;
  std::string truncated(session_key.substr(0, kMaxLen));
  if (session_key.size() > kMaxLen) {
    truncated.append("...");
  }
  return base::UTF8ToUTF16("Nexus: " + truncated);
}

// Resolve (Browser*, TabStripModel*, index, WebContents*) for a handle.
// `browser` is null when the tab is gone or its WebContents is
// orphaned.
struct LocatedTab {
  raw_ptr<Browser> browser = nullptr;
  raw_ptr<TabStripModel> tab_strip = nullptr;
  int index = TabStripModel::kNoTab;
  raw_ptr<content::WebContents> contents = nullptr;
};

LocatedTab LocateTabForHandle(tabs::TabHandle handle) {
  LocatedTab out;
  tabs::TabInterface* tab = handle.Get();
  if (!tab) return out;
  content::WebContents* contents = tab->GetContents();
  if (!contents) return out;
  out.contents = contents;
  out.browser = chrome::FindBrowserWithTab(contents);
  if (!out.browser) return out;
  out.tab_strip = out.browser->tab_strip_model();
  if (!out.tab_strip) {
    out.browser = nullptr;
    return out;
  }
  out.index = out.tab_strip->GetIndexOfWebContents(contents);
  return out;
}

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
  // Note: tabs are NOT closed here. Closure happens in ~BrowserTool
  // so that the OnStop-but-stay-warm reasons (kCompleted /
  // kUserStopped) preserve the user's tabs across turns.
}

void BrowserTool::BindTabToSession(tabs::TabHandle handle) {
  if (!::nexus::features::IsSessionScopedTabsEnabled() ||
      GetSessionKey().empty()) {
    return;
  }
  LocatedTab located = LocateTabForHandle(handle);
  if (!located.contents) {
    return;
  }
  ::nexus::NexusSessionTabTag::AttachTo(located.contents, GetSessionKey());

  // Add to (or create) the session's tab group. Best-effort: some
  // browser types (devtools, app windows) disable groups; that's fine,
  // the tag is still the source of truth. The visual group chrome is
  // kNexus-window-only: tabs hosted in a normal browsing window (the
  // separate-agent-window pref, or a user drag) stay ordinary-looking
  // tabs — the user asked for the session grouping to be invisible
  // there. The tag still associates them with the session.
  if (located.browser && located.tab_strip &&
      custom_browser::window_util::IsNexusBrowser(located.browser) &&
      located.tab_strip->SupportsTabGroups() &&
      located.index != TabStripModel::kNoTab) {
    if (tab_group_id_.has_value()) {
      std::optional<tab_groups::TabGroupId> current =
          located.tab_strip->GetTabGroupForTab(located.index);
      if (current != tab_group_id_) {
        located.tab_strip->AddToExistingGroup({located.index}, *tab_group_id_,
                                               /*add_to_end=*/true);
      }
    } else {
      tab_group_id_ = located.tab_strip->AddToNewGroup({located.index});
      if (TabGroup* group = located.tab_strip->group_model()->GetTabGroup(
              *tab_group_id_)) {
        tab_groups::TabGroupVisualData visual_data(
            TabGroupTitleFor(GetSessionKey()),
            DeterministicColorFor(GetSessionKey()),
            /*is_collapsed=*/false);
        group->SetVisualData(std::move(visual_data),
                              /*is_customized=*/true);
      }
      LOG(INFO) << "[BrowserTool] created tab group for session="
                << GetSessionKey()
                << " group=" << tab_group_id_->ToString();
    }
  }

  session_tabs_.insert(handle);

  // Keep this agent tab's renderer painting even when its (separate) window is
  // occluded behind the chat window — otherwise occlusion/background
  // throttling freezes it and observations/screenshots hang. A visible
  // capturer hold forces compositor frames regardless of occlusion
  // (web_contents.h), scoped to THIS WebContents only (user tabs untouched).
  // stay_hidden=false → page is treated visible; stay_awake=false → no
  // display wake-lock; is_activity=false → don't fake user activity.
  if (session_tab_capture_holds_.find(handle) ==
      session_tab_capture_holds_.end()) {
    session_tab_capture_holds_.emplace(
        handle, located.contents->IncrementCapturerCount(
                    gfx::Size(), /*stay_hidden=*/false,
                    /*stay_awake=*/false, /*is_activity=*/false));
  }

  // Nudge the profile-wide tracker: its TabStripModelObserver saw the
  // strip insert BEFORE the tag attach above, so the new tab was tallied
  // as untagged. This recount flips the session's counts and emits
  // `browser.sessionTabsChanged` to the guest (webview_area reveal).
  if (NexusSessionTabTracker* tracker =
          NexusSessionTabTrackerFactory::GetForProfile(GetProfile())) {
    tracker->NotifyTabBound(located.contents);
  }
}

void BrowserTool::RecordWorkTab(tabs::TabHandle handle) {
  // Verify the handle is still alive AND carries this session's tag
  // before storing — a working_tab_id resolved at the call site can
  // race a tab close on shutdown paths, and we don't want a stale
  // handle handed back by `LastWorkTab()` next call.
  tabs::TabInterface* tab = handle.Get();
  if (!tab) return;
  const std::string* tag =
      ::nexus::NexusSessionTabTag::SessionKeyFor(tab->GetContents());
  if (!tag || *tag != GetSessionKey()) {
    return;
  }
  last_work_tab_ = handle;
  // Mirror into the profile-wide tracker so `session.setActiveSession`
  // (handled on the page port, which cannot see this BrowserTool) can
  // honor "activate the last-worked tab" on session switch.
  if (NexusSessionTabTracker* tracker =
          NexusSessionTabTrackerFactory::GetForProfile(GetProfile())) {
    tracker->SetLastWorkTab(GetSessionKey(), handle);
  }
}

std::optional<tabs::TabHandle> BrowserTool::LastWorkTab() const {
  tabs::TabInterface* tab = last_work_tab_.Get();
  if (!tab) {
    return std::nullopt;
  }
  const std::string* tag =
      ::nexus::NexusSessionTabTag::SessionKeyFor(tab->GetContents());
  if (!tag || *tag != GetSessionKey()) {
    return std::nullopt;
  }
  return last_work_tab_;
}

std::vector<tabs::TabHandle> BrowserTool::CollectSessionTabs() const {
  std::vector<tabs::TabHandle> out;
  Profile* profile = GetProfile();
  if (!profile || GetSessionKey().empty()) {
    return out;
  }
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile) {
          return true;
        }
        TabStripModel* tab_strip = browser->tab_strip_model();
        if (!tab_strip) {
          return true;
        }
        for (int i = 0; i < tab_strip->count(); ++i) {
          content::WebContents* contents = tab_strip->GetWebContentsAt(i);
          const std::string* tag =
              ::nexus::NexusSessionTabTag::SessionKeyFor(contents);
          if (tag && *tag == GetSessionKey()) {
            tabs::TabInterface* tab = tab_strip->GetTabAtIndex(i);
            if (tab) {
              out.push_back(tab->GetHandle());
            }
          }
        }
        return true;
      });
  return out;
}

void BrowserTool::CloseAllSessionTabsOnDestroy() {
  if (!::nexus::features::IsSessionScopedTabsEnabled() ||
      GetSessionKey().empty()) {
    session_tab_capture_holds_.clear();
    session_tabs_.clear();
    tab_group_id_.reset();
    return;
  }
  // The tag — not session_tabs_ — is authoritative. A tab the user
  // dragged out of the group is still session-owned and gets closed.
  std::vector<tabs::TabHandle> tabs = CollectSessionTabs();
  LOG(INFO) << "[BrowserTool] closing tabs on destroy session="
            << GetSessionKey() << " count=" << tabs.size();
  for (const tabs::TabHandle& handle : tabs) {
    LocatedTab located = LocateTabForHandle(handle);
    if (!located.tab_strip || located.index == TabStripModel::kNoTab) {
      continue;
    }
    // CLOSE_USER_GESTURE keeps the close from being treated as a
    // crash-style teardown; CLOSE_CREATE_HISTORICAL_TAB lets the user
    // recover via Ctrl+Shift+T if they close the panel by mistake.
    located.tab_strip->CloseWebContentsAt(
        located.index,
        TabCloseTypes::CLOSE_USER_GESTURE |
            TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  }
  // Drop every capture hold (safe no-op for tabs already gone) — releases the
  // forced-render state before the tabs close.
  session_tab_capture_holds_.clear();
  session_tabs_.clear();
  tab_group_id_.reset();
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

  // Diagnostic: log EVERY state transition for our task, including
  // ones not handled by the pause / resume / terminal predicates.
  // Used to correlate task state changes with `tool_dropped` errors
  // — if the state never transitions to a terminal value between two
  // calls that the actor service dropped, the bug is somewhere other
  // than terminal-state-staleness.
  //
  // VLOG(1): opt-in via --vmodule=browser_tool=1+. State transitions
  // fire on every actor turn — too chatty for the default floor.
  VLOG(1) << "[BrowserTool] OnActorTaskStateChanged task_id="
          << task_id.value() << " state=" << static_cast<int>(state)
          << " session=" << GetSessionKey();

  // Terminal-state handling — aligned to upstream glic
  // (chrome/browser/glic/actor/glic_actor_task_manager.cc:655-669,
  // GlicActorTaskManager::NotifyActorTaskStateChanged).
  //
  // Once the actor task enters kFinished / kCancelled / kFailed it
  // never leaves (see chrome/browser/actor/actor_task.h:99-100). The
  // cached `task_id_` is forever stale at that point: a stale id
  // handed back from `EnsureTaskCreated` would steer the next
  // `PerformActions` into the actor service against a dead task, the
  // bound BindOnce gets silently dropped, and the inner
  // `ToolCallback`'s destructor emits the synthetic
  // `tool_dropped: tool dropped without resolving` error to the JS
  // guest — opaque to the model and unrecoverable without restart.
  //
  // Trigger we hit in production: a `BrowserClick` navigating a tab
  // cross-origin causes Chromium's actor framework to terminate the
  // task with `StoppedReason::kTabDetached`, transitioning the state
  // to `kCancelled`/`kFailed`. Without this branch the runtime kept
  // the stale id and every subsequent browser tool call returned
  // `tool_dropped`. See docs/agents/nexus-web-parity-audit.md
  // ("BrowserTool actor task lifecycle — adopted from glic").
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

  // custom-browser: Chromium 149 added a leading TaskSourceInfo argument.
  task_id_ = service->CreateTaskWithOptions(
      actor::TaskSourceInfo(actor::TaskSourceInfo::Client::kGlic,
                            /*id=*/std::nullopt),
      policy_checker, std::move(options),
      /*delegate=*/base::WeakPtr<actor::ActorTaskDelegate>());

  // Post-create validation. Turn-end `OnStop("completed")` calls
  // `service->StopTask(...)` on the previous task; the actor
  // service's async cleanup of that task can still be in flight
  // when the next conversation turn calls `CreateTaskWithOptions`
  // on a fresh `BrowserTool`. If the cleanup hasn't settled, the
  // new task can be returned with a null id, or returned as a
  // pointer that's not yet in the expected `kCreated` initial
  // state — in which case the very next `PerformActions` silently
  // drops its `BindOnce` and the inner ToolCallback fires
  // `tool_dropped` from its destructor.
  //
  // Convert that silent drop into a typed `task_create_*` reject
  // here so the model receives a recoverable error (and we get a
  // `LOG(WARNING)` for telemetry). Tracked in
  // `docs/agents/nexus-web-parity-audit.md` under "BrowserTool
  // restart after turn-end teardown".
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

  // Diagnostic: log the dispatch so we can correlate with whether
  // OnActorPerformActionsDone fires (or gets dropped). Pairs with the
  // entry log in OnActorPerformActionsDone and the
  // `[NexusToolCallback] Callback destroyed without resolving` warning
  // in nexus_tool_callback.cc — three together pinpoint where the
  // callback goes missing when `tool_dropped` reaches the JS guest.
  //
  // VLOG(1): opt-in via --vmodule=browser_tool=1+. Routine per-tool-
  // call line — would be too chatty at the default INFO floor.
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
    std::vector<actor::ActionResultWithLatencyInfo> action_results) {
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
  // Auto-recover from a wedged execution engine.
  //
  // `kExecutionEngineExistingAction` (code 37) means the actor's
  // ExecutionEngine refused to take a new action because the prior
  // one is still "in flight" — typically after a JS-side
  // bridge_timeout (60s) or a renderer crash (STATUS_BREAKPOINT) left
  // a phantom in-flight `Actions` request that never produced a
  // result. Without intervention, EVERY subsequent tool call in this
  // session keeps hitting 37 until the C++-side action_timeout (180s
  // default) finally fires — a 60-180s dead window during which the
  // model can do nothing. Even `BrowserCloseTab` re-hits 37 because
  // it dispatches through the same actor task; the runtime's
  // "close-and-reopen-the-tab-to-recover" advice for code 37 is
  // therefore unreachable from inside the model.
  //
  // The recovery is the same one the model is being asked to do
  // manually: stop the wedged task, reset cached id + subscription,
  // and surface a typed error that tells the model the host fixed
  // itself and the next call will mint a fresh task. The next tool
  // call's `EnsureTaskCreated` re-arms a healthy task; the model
  // loses ONE turn instead of being permanently stuck.
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
  // Defense-in-depth — aligned to upstream glic
  // (chrome/browser/glic/actor/glic_actor_task_manager.cc:161-167,
  // GlicActorTaskManager::PerformActionsFinished which synthesizes a
  // `kTaskWentAway` error when `!task` covers the in-state-map-but-
  // dead case our `!task` guard above misses).
  //
  // The actor service's task map keeps terminal-state tasks around
  // (see chrome/browser/actor/actor_task.h:99-100 — terminal states
  // are sticky; tasks aren't immediately evicted). So `GetTask`
  // returns a valid pointer to a dead task and the guard above
  // passes. Without this second check, `BuildActionsResultWithObservations`
  // walks a corpse, the actor framework silently drops the bound
  // BindOnce, and the inner ToolCallback's destructor emits the
  // synthetic `tool_dropped` error to the model — opaque and
  // unrecoverable. Reject with a typed `task_went_away` instead, and
  // proactively clear the cached id so the NEXT tool call goes
  // through `EnsureTaskCreated` and gets a fresh task.
  //
  // The Layer-1 fix in `OnActorTaskStateChanged` already clears
  // `task_id_` synchronously on state transitions; this branch
  // guards the race where this callback fires before the state-
  // change broadcast lands. Belt-and-suspenders, exactly like glic.
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
  // The upstream ObservationDelayController restarts its wait when a
  // navigation STARTS during it (kPageNavigated) — but "page settled" and a
  // renderer-initiated BeginNavigation travel on independent mojo pipes with
  // no cross-pipe ordering, so under CPU load the browser can process
  // "settled" while the click's navigation is still in transit. The APC
  // fetch then snapshots the OLD document and the model is told its click
  // did nothing (observed live: the e2e browser-tool sweep's click step
  // reporting the pre-navigation URL under a 6-worker parallel run).
  //
  // By the time the observation RESULT is built we have had a full renderer
  // roundtrip of wall time, so the raced navigation is browser-visible by
  // now in practice: either still in flight (IsLoading / a pending entry)
  // or already committed (live URL != the URL the observation captured).
  // On either signal, wait out the navigation (bounded) and re-observe
  // ONCE so the model sees the page its action actually produced. The
  // healthy path — observation matches the committed URL, nothing in
  // flight — pays nothing.
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
        // 300 × 150ms = a 45s bound, well inside the 180s per-action
        // budget. Generous on purpose: this path only runs when the
        // observation is ALREADY known-stale, and under the machine load
        // that creates this race a local navigation has been measured
        // needing 17s+ from click to first server request (e2e sweep,
        // 6-worker run). Real navigations rarely wait the full bound —
        // the poll exits at COMMIT (URL change), not full load-stop.
        // Better a late observation than a wrong one; a genuinely wedged
        // load still re-observes at exhaustion and resolves.
        // T0-c: the observation just built is NOT committed here — it is
        // only sent if the work tab dies mid-wait (the fallback path in
        // `WaitForWorkTabQuiescenceThenReobserve` commits it then);
        // otherwise the re-observe builds and commits its own.
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
    case apc::FORM_CONTROL_TYPE_INPUT_COLOR:
      return "color";
    case apc::FORM_CONTROL_TYPE_INPUT_DATE:
      return "date";
    case apc::FORM_CONTROL_TYPE_INPUT_DATETIME_LOCAL:
      return "datetime-local";
    case apc::FORM_CONTROL_TYPE_INPUT_EMAIL:
      return "email";
    case apc::FORM_CONTROL_TYPE_INPUT_FILE:
      // The model needs this to find the upload control for BrowserUpload;
      // before, file inputs carried no input_type at all.
      return "file";
    case apc::FORM_CONTROL_TYPE_INPUT_MONTH:
      return "month";
    case apc::FORM_CONTROL_TYPE_INPUT_NUMBER:
      return "number";
    case apc::FORM_CONTROL_TYPE_INPUT_PASSWORD:
      return "password";
    case apc::FORM_CONTROL_TYPE_INPUT_RADIO:
      return "radio";
    case apc::FORM_CONTROL_TYPE_INPUT_RANGE:
      return "range";
    case apc::FORM_CONTROL_TYPE_INPUT_SEARCH:
      return "search";
    case apc::FORM_CONTROL_TYPE_INPUT_TELEPHONE:
      return "tel";
    case apc::FORM_CONTROL_TYPE_INPUT_TEXT:
      return "text";
    case apc::FORM_CONTROL_TYPE_INPUT_TIME:
      return "time";
    case apc::FORM_CONTROL_TYPE_INPUT_URL:
      return "url";
    case apc::FORM_CONTROL_TYPE_INPUT_WEEK:
      return "week";
    case apc::FORM_CONTROL_TYPE_SELECT_ONE:
      return "select";
    case apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE:
      return "select-multiple";
    case apc::FORM_CONTROL_TYPE_TEXT_AREA:
      return "textarea";
    // <input type="button|submit|reset"> AND <button type="button|submit|
    // reset|popover">. The model previously saw no `input_type` AND
    // role:"textbox" for these, hiding submit/CTA buttons inside busy
    // forms behind the same shape as text fields. Empirically reproduced
    // on AKC's RETRIEVE EVENTS button (`<input type="button"
    // value="RETRIEVE EVENTS" class="retrieve-events">`): the model
    // surfaced the index but dismissed it as "not the button" because
    // role read "textbox". Mapping to the HTML type string here +
    // returning "button" from ExtractRole below makes the entry
    // unambiguous on the wire.
    case apc::FORM_CONTROL_TYPE_INPUT_BUTTON:
    case apc::FORM_CONTROL_TYPE_BUTTON_BUTTON:
      return "button";
    case apc::FORM_CONTROL_TYPE_INPUT_SUBMIT:
    case apc::FORM_CONTROL_TYPE_BUTTON_SUBMIT:
      return "submit";
    case apc::FORM_CONTROL_TYPE_INPUT_RESET:
    case apc::FORM_CONTROL_TYPE_BUTTON_RESET:
      return "reset";
    case apc::FORM_CONTROL_TYPE_BUTTON_POPOVER:
      return "popover";
    // <input type="image"> — a clickable image submit button.
    // Common pattern: logo-as-submit on e-commerce sites, magnifying-
    // glass icon submitting a search form. Same bug class as the
    // button family above: emits role:"textbox" with no input_type
    // if we don't handle it. Surfaces as input_type:"image" so the
    // model can recognize the icon-button pattern.
    case apc::FORM_CONTROL_TYPE_INPUT_IMAGE:
      return "image";
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
    // <input type="button|submit|reset"> AND <button type="button|submit
    // |reset|popover">. Surface as role:"button" so the model recognizes
    // submit/CTA controls as actuators rather than mistaking them for
    // text fields. The empirical AKC failure was caused by RETRIEVE
    // EVENTS surfacing as role:"textbox" — fixed by recognizing the
    // button family here.
    if (t == apc::FORM_CONTROL_TYPE_INPUT_BUTTON ||
        t == apc::FORM_CONTROL_TYPE_INPUT_SUBMIT ||
        t == apc::FORM_CONTROL_TYPE_INPUT_RESET ||
        t == apc::FORM_CONTROL_TYPE_INPUT_IMAGE ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_BUTTON ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_SUBMIT ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_RESET ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_POPOVER) {
      return "button";
    }
    return "textbox";  // text / email / password / url / number / tel /
                       // contenteditable all behave as text input for
                       // the LLM's purposes
  }
  if (attrs.has_interaction_info() &&
      attrs.interaction_info().is_clickable()) {
    return "button";
  }
  // Clickable images. Empirically `<input type="image">` reaches this
  // function as `CONTENT_ATTRIBUTE_IMAGE` (NOT `FORM_CONTROL` — APC
  // emits the input as an image node, the form-control sibling node
  // either isn't produced or is filtered out before elements[]) and
  // typically without `interaction_info.is_clickable()` set on the
  // image node itself, so neither of the branches above fires and the
  // element would otherwise emit as a bare `{index, type:"image"}` —
  // hard to distinguish from a decorative <img>. IsActionable already
  // accepted it (via `is_focusable` or `aria_role`), so it IS an
  // actuator — surface it to the model as a button. Also catches
  // `<img onclick>` / `<svg role="img" onclick>` icon-button patterns.
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_IMAGE) {
    return "button";
  }
  // ARIA-only interactive widgets: the node is neither ANCHOR nor
  // FORM_CONTROL and Blink didn't flag it as is_clickable (typically
  // because the React/Vue framework delegates the click on the parent
  // listbox and the child <li> has tabindex=-1 / cursor:auto). Surface
  // the ARIA role string so the model sees `role: "option"` instead of
  // a bare `{index, type:"list_item"}` it can't distinguish from a
  // generic <li>. The allowlist + emitted role-string is the same
  // shared `RoleStringFromAXRole` table that `IsActionable` checks —
  // single source of truth, so adding a new role there automatically
  // emits the matching role string here too.
  if (attrs.has_aria_role()) {
    if (const char* role = RoleStringFromAXRole(attrs.aria_role())) {
      return role;
    }
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

void BrowserTool::CollectLabelTargets(const apc::ContentNode& node) {
  // Record, for every `<label for=X>` node with a non-empty visible rect,
  // the mapping `X (the labelled input's dom_node_id) → {label's own
  // node_id, label box}`. APC sets `label_for_dom_node_id` on the label
  // node and guarantees it equals the input's `common_ancestor_dom_node_id`
  // (see common_quality_data.proto). This lets `WalkContentNode` retarget
  // a hidden checkbox/radio (custom "sr-only" input behind a styled label,
  // e.g. Uniqlo's size/colour filters) to its visible, clickable label.
  // First label with a visible box wins (labels are 1:1 with inputs in
  // practice).
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    if (attrs.has_label_for_dom_node_id() &&
        attrs.has_common_ancestor_dom_node_id() && attrs.has_geometry() &&
        attrs.geometry().has_visible_bounding_box()) {
      const apc::BoundingRect& r = attrs.geometry().visible_bounding_box();
      if (r.width() > 0 || r.height() > 0) {
        const int input_id = attrs.label_for_dom_node_id();
        // First label with a visible box wins — keep it if already set.
        if (observation_label_targets_.find(input_id) ==
            observation_label_targets_.end()) {
          // Capture the label's visible text ("Honda") so WalkContentNode can
          // surface it on an otherwise-unlabeled control. The text may be on
          // the label node itself or nested in a child span.
          std::string label_text = ExtractNodeText(attrs);
          if (label_text.empty()) {
            label_text = ExtractDescendantTextSnippet(node);
          }
          observation_label_targets_.insert_or_assign(
              input_id, LabelTarget{attrs.common_ancestor_dom_node_id(),
                                    {r.x(), r.y(), r.width(), r.height()},
                                    std::move(label_text)});
        }
      }
    }
  }
  for (const apc::ContentNode& child : node.children_nodes()) {
    CollectLabelTargets(child);
  }
}

void BrowserTool::WalkContentNode(const apc::ContentNode& node,
                                   const std::string& document_id,
                                   int tab_id,
                                   base::ListValue& out_elements) {
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    // Skip emitting a standalone `<label for=...>` node: its clickable
    // role is folded into the control it labels (a hidden checkbox/radio
    // gets RE-TARGETED to this label below), so emitting it separately
    // would produce a duplicate element pointing at the same DOM node.
    // Children still recurse at the bottom so inner text/elements surface.
    if (IsActionable(attrs) && attrs.has_common_ancestor_dom_node_id() &&
        !attrs.has_label_for_dom_node_id()) {
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
      // Still no accessible name? Borrow the associated `<label for=...>`'s
      // text. A form control whose label is a separate element (the input and
      // its name are siblings, not nested) otherwise emits as a bare
      // `{index, input_type:checkbox}` — so on a filter list of identical
      // checkboxes the model can't tell which is "Honda" and resorts to
      // guessing by position. Surfacing the label text lets it target the
      // control by name. (Keyed by node_id: APC guarantees the label's
      // `label_for_dom_node_id` equals the input's `common_ancestor_dom_node_id`.)
      if (text.empty()) {
        auto lt = observation_label_targets_.find(node_id);
        if (lt != observation_label_targets_.end() &&
            !lt->second.text.empty()) {
          text = lt->second.text;
        }
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
        // Native <select> options. APC carries the full option list (value +
        // text) in `select_options`; without surfacing it the model sees only
        // the currently-selected label (`value` above) and can neither
        // DISCOVER that other options exist nor BrowserSelectOption one — that
        // tool needs the exact `value` attribute, which clicking a native
        // <select> can't reveal (the popup is OS-rendered, not DOM). Emit
        // `options: [{value, text, selected?, disabled?}]` so the model can
        // match a visible label to its value and select it. Capped so a huge
        // <select> (country picker ~250) can't bloat the observation;
        // `options_truncated: true` flags when the list was cut.
        if (fc.select_options_size() > 0) {
          constexpr int kMaxSelectOptions = 200;
          const int total = fc.select_options_size();
          const int emit_count =
              total < kMaxSelectOptions ? total : kMaxSelectOptions;
          base::ListValue options;
          for (int i = 0; i < emit_count; ++i) {
            const apc::SelectOption& opt = fc.select_options(i);
            base::DictValue o;
            o.Set("value", opt.value());
            if (!opt.text().empty()) {
              o.Set("text", opt.text());
            }
            if (opt.is_selected()) {
              o.Set("selected", true);
            }
            if (opt.is_disabled()) {
              o.Set("disabled", true);
            }
            options.Append(std::move(o));
          }
          elem.Set("options", std::move(options));
          if (total > emit_count) {
            elem.Set("options_truncated", true);
          }
        }
      }
      // Anchor specifics — destination URL. Critical for telling search
      // result links apart on a results page.
      if (attrs.has_anchor_data() && !attrs.anchor_data().url().empty()) {
        // T0-a: slim the emitted href — drop scheme+host for same-origin
        // links, strip tracking params, cap length. `last_observation_url_`
        // is set from main_frame_data before this walk runs, so it names
        // the working tab's page. The dom_hash projection above keeps the
        // RAW href (`h=`), so this slimming never perturbs the fingerprint.
        bool href_truncated = false;
        elem.Set("href",
                 browser_tool_internal::CompressHref(
                     attrs.anchor_data().url(), last_observation_url_,
                     &href_truncated));
        if (href_truncated) {
          elem.Set("href_truncated", true);
        }
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

      // Hidden-control retarget. A checkbox/radio whose own rect is empty
      // is a CSS-hidden ("sr-only") custom control behind a styled
      // `<label for=...>`. The actor can't click the input (empty rect →
      // kElementOffscreen) and scroll_to can't help. If the pre-pass found
      // the associated label and it HAS a visible box, retarget this
      // element to the label's node_id + box: clicking the label toggles
      // the input via native HTML, so the LLM gets a working, box-bearing
      // control instead of an un-clickable offscreen one. We keep the
      // input's role / input_type / name / is_checked (set above) so the
      // model still sees it as a checkbox/radio and its current state.
      if (!box && attrs.has_form_control_data()) {
        const apc::FormControlType fct =
            attrs.form_control_data().form_control_type();
        if (fct == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
            fct == apc::FORM_CONTROL_TYPE_INPUT_RADIO) {
          auto lt = observation_label_targets_.find(node_id);
          if (lt != observation_label_targets_.end()) {
            const int label_node_id = lt->second.node_id;
            base::ListValue lbox;
            lbox.Append(lt->second.box[0]);
            lbox.Append(lt->second.box[1]);
            lbox.Append(lt->second.box[2]);
            lbox.Append(lt->second.box[3]);
            box = base::Value(std::move(lbox));
            // Register the label node_id too, so a later lookup by the
            // emitted index resolves to the same entry as the input.
            node_registry_.Set(label_node_id, entry);
            // Overwrite the index set earlier with the clickable label's
            // node_id so a BrowserClick lands on the visible label.
            elem.Set("index", label_node_id);
          }
        }
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
    const apc::ActionsResult& result_proto,
    bool include_screenshot_image,
    bool force_full) {
  // A projection staged by an earlier walk that was never resolved (e.g.
  // the settle guard dropped that observation) is dead — this walk stages
  // its own, and only the caller resolving THIS result commits.
  observation_baseline_.Discard();
  // The "working tab" is the session's stored `last_work_tab_` — set
  // by `RecordWorkTab` at the top of `RunActorAction` /
  // `RunTabObservation`, so by the time we're building the
  // post-action observation it points at the tab the just-dispatched
  // action ran against. Stateful by design: any future tab-less tool
  // call also reads it via `GetSessionScopedActiveTabHandleId`, so
  // the LLM's "current focus" lives in one place and the observation
  // mirrors it.
  //
  // Only the working tab gets the full APC walk, `elements[]`
  // payload, screenshot flag, `last_observation_*` snapshot, and
  // `node_registry_` updates. Every other session-owned tab in the
  // observation gets basics only — `tab_id` / `url` / `title`. The
  // LLM must `BrowserActivateTab(id)` to bring a background tab into
  // focus and see its interactive elements.
  //
  // `LastWorkTab()` returns nullopt when no tool action has dispatched
  // yet OR the recorded tab has died (e.g. just-closed by `close_tab`).
  // In that case every entry is basics-only and the
  // `last_observation_*` snapshot stays cleared.
  const std::optional<tabs::TabHandle> working_handle = LastWorkTab();
  const std::optional<int32_t> working_tab_id =
      working_handle.has_value()
          ? std::make_optional(working_handle->raw_value())
          : std::nullopt;
  last_observation_url_.clear();
  last_observation_title_.clear();
  last_observation_interactive_count_ = 0;
  last_observation_tab_id_.reset();
  last_observation_normalized_projection_.clear();
  last_observation_focus_target_present_ = false;
  last_observation_focus_target_id_.clear();

  // When session-scoped tabs are enabled, observations must hide tabs
  // owned by other sessions (or by the user outside any session). This
  // is the single choke point — every tool's observation flows through
  // here, so a filter here is sufficient to strictly hide cross-session
  // tabs from the model's view.
  const bool session_scope =
      ::nexus::features::IsSessionScopedTabsEnabled() &&
      !GetSessionKey().empty();
  const std::string this_session_key = GetSessionKey();

  base::DictValue observation;
  base::ListValue tabs;
  for (const apc::TabObservation& tab_obs : result_proto.tabs()) {
    if (session_scope) {
      tabs::TabInterface* tab_iface = tabs::TabHandle(tab_obs.id()).Get();
      content::WebContents* contents =
          tab_iface ? tab_iface->GetContents() : nullptr;
      const std::string* tag =
          ::nexus::NexusSessionTabTag::SessionKeyFor(contents);
      if (!tag || *tag != this_session_key) {
        continue;
      }
    }

    const bool is_working = working_tab_id.has_value() &&
                            tab_obs.id() == *working_tab_id;

    base::DictValue tab_dict;
    tab_dict.Set("tab_id", tab_obs.id());

    // Surface observation-fetch failures LOUDLY instead of silently
    // emitting an empty tab. Upstream's FetchCallback
    // (chrome/browser/actor/actor_proto_conversion.cc) sets
    // `result = TAB_OBSERVATION_FETCH_ERROR` and returns WITHOUT
    // attaching `annotated_page_content` when the APC fetch or a
    // required screenshot capture fails — so without this field the
    // model (and every e2e assertion) sees `elements: []` +
    // `page_text: ""` with `ok: true` and misreads a broken
    // observation as an empty page. The sub-result codes say which
    // leg failed (APC vs screenshot).
    if (tab_obs.result() != apc::TabObservation::TAB_OBSERVATION_OK) {
      tab_dict.Set(
          "observation_error",
          base::StrCat(
              {"tab observation failed: result=",
               apc::TabObservation::TabObservationResult_Name(
                   tab_obs.result()),
               " apc=",
               apc::TabObservation::AnnotatedPageContentResult_Name(
                   tab_obs.annotated_page_content_result()),
               " screenshot=",
               apc::TabObservation::ScreenshotResult_Name(
                   tab_obs.screenshot_result())}));
    }

    // Pull `url` / `title` from main_frame_data — emitted for every
    // tab (basics for non-working, full envelope's basics for the
    // working one too).
    if (tab_obs.has_annotated_page_content() &&
        tab_obs.annotated_page_content().has_main_frame_data()) {
      const apc::FrameData& mf =
          tab_obs.annotated_page_content().main_frame_data();
      if (!mf.url().empty()) {
        tab_dict.Set("url", mf.url());
      }
      if (!mf.title().empty()) {
        tab_dict.Set("title", mf.title());
      }
    }

    if (!is_working) {
      // Non-working tab — basics only. No elements walk, no screenshot,
      // no node_registry / projection updates.
      tabs.Append(std::move(tab_dict));
      continue;
    }

    // ── Working tab: full payload ─────────────────────────────────
    // Deliver the actual pixels ONLY for BrowserScreenshot. Every other action
    // omits the image entirely: the structured observation already lists every
    // actionable element, and shipping a base64 image on every action would
    // bloat each result with pixels the model didn't ask for. We intentionally
    // emit NO availability flag — the model can call BrowserScreenshot whenever
    // it needs pixels regardless of any marker, and a bare `has_screenshot`
    // flag read as if the image were attached (it wasn't), so it was removed.
    // The presence of the `screenshot` key is the unambiguous "image included"
    // signal. (`tab_obs.has_screenshot()` below is the APC proto getter, not a
    // result field.)
    if (include_screenshot_image && tab_obs.has_screenshot() &&
        !tab_obs.screenshot().empty()) {
      // The bridge browser-result transport is JSON-only, and the model API
      // takes base64 regardless, so base64 here is the right carrier — JS
      // lifts it straight into an image content block. `screenshot` is raw
      // bytes; `screenshot_mime_type` is the captured format (defaults to JPEG
      // when the framework didn't set one).
      base::DictValue shot;
      shot.Set("mime", (tab_obs.has_screenshot_mime_type() &&
                        !tab_obs.screenshot_mime_type().empty())
                           ? tab_obs.screenshot_mime_type()
                           : std::string("image/jpeg"));
      shot.Set("data_base64", base::Base64Encode(tab_obs.screenshot()));
      tab_dict.Set("screenshot", std::move(shot));
    }

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

      // Populate the agent_meta snapshot from the working tab.
      last_observation_tab_id_ = tab_obs.id();
      if (apc_data.has_main_frame_data()) {
        last_observation_url_ = apc_data.main_frame_data().url();
        last_observation_title_ = apc_data.main_frame_data().title();
      }

      // Stable focus signal for multi-act guards. The proto's
      // `page_interaction_info.focused_node_id` is the focused
      // element's DOM content_node_id — same identifier space as
      // the LLM-facing index, so multi-act guards, focus tracking,
      // and registry lookups all share the same key.
      if (apc_data.has_page_interaction_info() &&
          apc_data.page_interaction_info().has_focused_node_id() &&
          apc_data.page_interaction_info().focused_node_id() != 0) {
        last_observation_focus_target_present_ = true;
        last_observation_focus_target_id_ = base::NumberToString(
            apc_data.page_interaction_info().focused_node_id());
      }

      const int tab_id_for_registry =
          apc_data.has_tab_id() ? apc_data.tab_id() : tab_obs.id();

      if (apc_data.has_root_node()) {
        // Evict stale registry entries when this tab's document changed
        // since we last walked it — their content_node_ids belong to a
        // defunct document and would otherwise resolve to actor-rejected
        // targets. (NodeRegistry::ClearTab wiring.)
        auto doc_it = registry_document_by_tab_.find(tab_id_for_registry);
        if (doc_it != registry_document_by_tab_.end() &&
            doc_it->second != document_id) {
          node_registry_.ClearTab(tab_id_for_registry);
        }
        registry_document_by_tab_[tab_id_for_registry] = document_id;

        // Pre-pass: index `<label for=...>` → control so `WalkContentNode`
        // can retarget hidden checkboxes/radios to their visible label.
        // Per-tab scope — clear before each walk.
        observation_label_targets_.clear();
        CollectLabelTargets(apc_data.root_node());
        WalkContentNode(apc_data.root_node(), document_id, tab_id_for_registry,
                        elements);
      }

      // T0-c: short-circuit an unchanged working-tab observation. The walk
      // above already refreshed node_registry_ and the last_observation_*
      // snapshot (so indexes still resolve and agent_meta is correct); we
      // only replace the bulky elements[] payload with a compact stub when
      // the interactive surface is byte-identical to the last full send on
      // the same document. Never for BrowserScreenshot (the model asked for
      // pixels + a fresh look) and never when the caller explicitly asked
      // for the elements (`force_full` — `include_observation:true`).
      const bool short_circuit =
          !include_screenshot_image && !force_full &&
          observation_baseline_.ShouldShortCircuit(
              observation_diff_enabled_, document_id,
              last_observation_normalized_projection_);
      if (short_circuit) {
        base::DictValue unchanged;
        unchanged.Set("unchanged", true);
        unchanged.Set("count", static_cast<int>(elements.size()));
        tab_dict.Set("elements", std::move(unchanged));
        tabs.Append(std::move(tab_dict));
        continue;
      }
      // Full walk — STAGE this projection/doc. It becomes the baseline
      // only when the resolving caller commits it (the elements[] really
      // went to the model); a caller that strips the observation to basics
      // or throws it away discards instead, so the baseline keeps tracking
      // what is actually in the model's context.
      observation_baseline_.Stage(document_id,
                                  last_observation_normalized_projection_);
    }
    tab_dict.Set("elements", std::move(elements));
    tabs.Append(std::move(tab_dict));
  }

  observation.Set("tabs", std::move(tabs));

  // Drain any out-of-band notices recorded on the working tab since the last
  // observation — e.g. a native file dialog the RunFileChooser hook suppressed
  // on this agent tab — so the model sees facts the page may have swallowed.
  // Sticky until drained: the chooser fires asynchronously relative to the
  // action that caused it, so a notice can surface on a later observation than
  // the click that triggered it.
  if (working_handle.has_value()) {
    tabs::TabInterface* work_tab = working_handle->Get();
    content::WebContents* work_contents =
        work_tab ? work_tab->GetContents() : nullptr;
    NexusPendingAgentNotice* pending =
        work_contents
            ? NexusPendingAgentNotice::FromWebContents(work_contents)
            : nullptr;
    if (pending) {
      std::vector<AgentNotice> drained = pending->TakeAll();
      if (!drained.empty()) {
        base::ListValue notices;
        for (const AgentNotice& notice : drained) {
          notices.Append(notice.ToDict());
        }
        observation.Set("notices", std::move(notices));
      }
    }
  }
  return observation;
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
    {
      base::DictValue reject_params;
      reject_params.Set("tool", tool_name);
      reject_params.Set("index", node_id);
      if (tab_id.has_value()) {
        reject_params.Set("tab_id", *tab_id);
      }
      std::move(done).Reject("index_not_found", std::move(reject_params));
    }
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

