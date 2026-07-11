#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_INTERNAL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_INTERNAL_H_

// Internal-only header shared between browser_tool.cc + browser_tool_actions.cc.
// Holds the cross-cutting helpers + private classes that used to live in
// browser_tool.cc's anonymous namespace; promoted to a namespace with
// external linkage so the .cc-file split (lifecycle vs tool methods)
// can share them. NOT part of the public API — nothing outside
// nexus/tools/browser_tool/ should include this.
//
// Why split BrowserTool across two .cc files at all:
// browser_tool.cc grew past 100KB while it absorbed every tool body
// post-Phase-8b. The split groups infrastructure (actor pipeline,
// observation builder, lifecycle, EnsureTaskCreated, LookupNode) in
// browser_tool.cc, leaving the 24 LLM-facing tool method bodies +
// the create-tab + observation-completion helpers in
// browser_tool_actions.cc. Each file is ~half the size and easier to
// navigate.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/common/actor.mojom-forward.h"
#include "components/optimization_guide/proto/features/actions_data.pb.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"

class Browser;
class Profile;

namespace actor {
class EnterprisePolicyChecker;
struct ActionResultWithLatencyInfo;
}

namespace nexus {
class BrowserTool;
}  // namespace nexus

namespace nexus::browser_tool_internal {

namespace apc = optimization_guide::proto;

// Wall-clock budget guard with pause/resume support. Defined here
// (instead of in a single .cc) because both browser_tool.cc (via
// `BrowserTool::active_timeouts_` member iteration) and the
// `BrowserTool::PausableBudgetWrap` plumbing need the full type.
class PausableTimeout : public base::RefCounted<PausableTimeout> {
 public:
  // Chromium-style flags inlined ctor/dtor on classes with non-trivial
  // members (std::string, OnceClosure, OneShotTimer) as "complex" — both
  // are out-of-line in browser_tool_internal.cc.
  PausableTimeout(base::TimeDelta budget,
                  std::string name,
                  base::OnceClosure on_timeout);

  void Start(bool paused);
  void Pause();
  void Resume();
  void Stop();

  bool settled() const { return settled_; }

 private:
  friend class base::RefCounted<PausableTimeout>;
  ~PausableTimeout();

  void ArmTimer();
  void Fire();

  base::TimeDelta remaining_;
  std::string name_;
  base::OnceClosure on_timeout_;
  base::OneShotTimer timer_;
  base::TimeTicks started_at_;
  bool paused_ = false;
  bool settled_ = false;
};

// Wrapper around a tool's `done` callback that integrates with a
// PausableTimeout — used by `BrowserTool::PausableBudgetWrap` to
// give each outgoing actor call a settle-once-or-fail-fast budget.
class PausableToolCallbackHolder
    : public base::RefCounted<PausableToolCallbackHolder> {
 public:
  PausableToolCallbackHolder(::nexus::ToolCallback inner,
                             std::string action_name);

  void SetTimeout(scoped_refptr<PausableTimeout> timeout);
  void Resolve(base::Value value);
  void Reject(std::string code, std::string message);
  void OnTimeout();

 private:
  friend class base::RefCounted<PausableToolCallbackHolder>;
  ~PausableToolCallbackHolder();

  ::nexus::ToolCallback inner_;
  std::string action_name_;
  scoped_refptr<PausableTimeout> timeout_;
};

// =============================================================================
// Cross-cutting free helpers. Functions only used inside one split .cc
// stay in that file's anonymous namespace; this header captures the
// ones called from at least two of the split files.
// =============================================================================

// Production EnterprisePolicyChecker getter. Used by EnsureTaskCreated.
const actor::EnterprisePolicyChecker* GetPolicyChecker(Profile* profile);

// Map ActionResultCode → "actor_error_<N>: <human description>".
// Used by OnActionsResultBuilt + the read_page / find_text completions.
std::string DescribeActorError(actor::mojom::ActionResultCode code);

// Tool-aware variant of `DescribeActorError`. Overrides the description
// for `(action_name, code)` combinations where the generic message would
// mislead the model.
//
// Specifically: when a scroll-tool (`scroll_to` / `scroll`) itself
// returns `kElementOffscreen`, the standard message ("prepend a
// scroll_to action targeting that index, then retry") is wrong — the
// model would just re-issue the same scroll_to and loop. Upstream
// Chromium's `ToolBase::EnsureTargetInView`
// (`chrome/renderer/actor/tool_base.cc:224-258`) calls
// `Element.ScrollIntoViewIfNeeded()`, which silently does nothing when
// the target is clipped by an `overflow:hidden` ancestor (common in
// card-style search-result carousels). The post-scroll validation then
// still sees empty `VisibleBoundsInWidget()` and re-emits
// `kElementOffscreen`. CLAUDE.md restricts upstream edits, so this
// helper rewrites the error message at the bridge boundary instead —
// telling the model to switch tactics (BrowserFindText, or pick a
// different element) rather than retry the same call.
//
// For any combination this helper doesn't special-case, it delegates
// straight to `DescribeActorError(code)` — keeping behavior identical
// for click/input/etc. paths where the original "scroll first" advice
// is correct.
std::string DescribeActorErrorForTool(std::string_view action_name,
                                       actor::mojom::ActionResultCode code);

// Reduce a per-action result vector to one overall `ActionResultCode`.
//
// Chromium 149 (crrev "149") removed the standalone `result_code` /
// `index_of_failed_action` arguments from the `PerformActions` completion
// callback and `BuildActionsResultWithObservations`, folding them into the
// per-action `action_results` vector. This delegates to upstream's own
// canonical reducer, `actor::ExtractErrorResult` (see
// `chrome/common/actor/action_result.h`): the FIRST non-OK action's code
// wins, else `kOk`. That is exactly the rule the actor framework itself
// uses to surface an overall error, and it matches the engine's
// stop-at-first-failure execution — so it is correct for both the
// single-action requests Nexus issues today (click / navigate / scroll /
// …) and the multi-action `fill_form` batch (which the engine halts at the
// first failing field). Using the upstream helper rather than a local copy
// keeps the two in lockstep across future core upgrades.
actor::mojom::ActionResultCode OverallActorResultCode(
    const std::vector<actor::ActionResultWithLatencyInfo>& action_results);

// Map a typed `OnStopReason` to `actor::ActorTask::StoppedReason`.
// Exhaustive over the enum so any new reason added to OnStopReason
// forces an explicit decision here.
actor::ActorTask::StoppedReason ToActorStoppedReason(OnStopReason reason);

// Most-recently-active Browser whose Profile matches `profile`, or
// nullptr.
Browser* GetActiveBrowserForProfile(Profile* profile);

// SessionID (int32) of the active window, or std::nullopt.
std::optional<int32_t> GetActiveWindowSessionId(Profile* profile);

// Resolves the SessionID of the window a newly created agent tab should be
// placed in, honoring the `nexus.browser_tools_separate_window` pref:
//   * pref off (default) — the active window (same as
//     GetActiveWindowSessionId).
//   * pref on            — a dedicated separate window, reused across calls
//     and created on demand if one does not yet exist for the profile.
// Returns std::nullopt when no suitable window is available / could be
// created.
std::optional<int32_t> ResolveCreateTabWindowSessionId(Profile* profile);

// `tabs::TabHandle::raw_value()` of the active tab, or std::nullopt.
std::optional<int32_t> GetActiveTabHandleId(Profile* profile);

// Session-scoped variant: returns the most-recently-active tab among
// the session's own tabs (per `NexusSessionTabTag`), or the first tab
// owned by the session if no last-active is recorded. Returns
// std::nullopt when the session owns no tabs.
//
// Reads session-tab state from the BrowserTool member fields (the
// pre-refactor profile-scoped registry has been removed). When
// `browser_tool` is null OR the feature flag is off, falls back to
// `GetActiveTabHandleId`.
std::optional<int32_t> GetSessionScopedActiveTabHandleId(
    ::nexus::BrowserTool* browser_tool);

// Resolves the explicit-or-active tab id for a tool method; rejects
// `done` with `no_active_tab` / `no_session_tab` / `tab_not_in_session`
// and returns std::nullopt on miss. Caller MUST early-return on miss
// (don't read or move `done` afterward).
//
// `browser_tool` enables the per-session enforcement when non-null AND
// `kSessionScopedTabs` is on:
//   * explicit tab_id present → checks `NexusSessionTabTag` matches
//     the BrowserTool's session_key; rejects with `tab_not_in_session`
//     otherwise.
//   * explicit tab_id absent → resolves via
//     `GetSessionScopedActiveTabHandleId(browser_tool)`; rejects with
//     `no_session_tab` if the session owns no tabs.
// A null `browser_tool` (or the flag off) falls back to the legacy
// behavior — explicit id passes through, no id falls back to
// `GetActiveTabHandleId`.
std::optional<int32_t> ResolveTabIdOrReject(
    std::optional<int> explicit_tab_id,
    Profile* profile,
    std::string_view tool_name,
    ::nexus::ToolCallback& done,
    ::nexus::BrowserTool* browser_tool = nullptr);

// Prepend an ActivateTab proto action to `actions` if `target_tab_id`
// isn't already the active tab. No-op when already-active or when the
// active tab can't be resolved.
void MaybePrependActivateTab(apc::Actions* actions,
                             int32_t target_tab_id,
                             Profile* profile);

// =============================================================================
// Tree-walking helpers. Used by both the observation builder (in
// browser_tool.cc) and the find_text walker (in browser_tool_actions.cc).
// =============================================================================

// Maps the proto's CONTENT_ATTRIBUTE_* enum onto the short type names
// the LLM expects ("link", "form_control", …).
const char* ContentAttributeTypeName(apc::ContentAttributeType t);

// Pulls the best visible-text snippet out of a node's attributes
// (label first, then text_data).
std::string ExtractNodeText(const apc::ContentAttributes& attrs);

// True if the node should appear in the observation as actionable
// (clickable/focusable/form-control/anchor).
bool IsActionable(const apc::ContentAttributes& attrs);

// Trims `s` so it ends on a UTF-8 character boundary, preventing
// base::Value's UTF-8 validator from CHECK-failing on a half-truncated
// multi-byte sequence (Chinese / Japanese / Korean / emoji).
std::string TruncateUtf8Snippet(std::string s, int max_bytes);

// Cap on per-element descendant-text snippet (used by the observation
// builder's walker fallback).
inline constexpr int kDescendantTextSnippetMax = 80;

}  // namespace nexus::browser_tool_internal

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_INTERNAL_H_
