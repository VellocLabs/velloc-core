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
#include "base/values.h"
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
  void Reject(std::string code, base::DictValue params);
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

// `params` for a browser-family error: `{action?, phase?}`, `{phase}`,
// `{reason}`. The runtime renders the model-visible sentence from the
// code and these params; C++ carries no prose.
base::DictValue ActionParams(std::string_view action,
                             std::string_view phase = std::string_view());
base::DictValue PhaseParams(std::string_view phase);
base::DictValue ReasonParams(std::string_view reason);

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

// Single source of truth for the APC AXRole → ARIA role-string mapping.
// Returns a stable role string for a recognized interactive ARIA widget
// role, or nullptr for any AXRole not in the allowlist.
//
// Used by BOTH `IsActionable` (treats non-null as "this widget is
// actionable, emit it in elements[]") and `ExtractRole` (uses the
// returned string as the entry's `role` field). Adding a new role here
// automatically extends both sites in lockstep. This consolidation
// closes the drift class of bugs whose mirror on the FORM_CONTROL side
// surfaced `<input type="button">` as `role:"textbox"` with no
// `input_type` (the IsActionable AXRole allowlist used to be a
// hand-duplicated switch alongside ExtractRole's identical-by-shape
// switch — same family of risk).
//
// Allowlist policy: ARIA widget roles that imply a clear "click /
// activate / select" interaction with no required value-entry,
// arrow-key, or popup semantics that the model would also need to
// drive. Roles needing richer interaction (slider, spinbutton,
// combobox*, searchbox, disclosure-triangle expanded-state,
// menubutton popup management) are intentionally NOT included — they
// require richer metadata than just `role` exposure, deferred to a
// follow-up patch with proper signals.
const char* RoleStringFromAXRole(apc::AXRole role);

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

// =============================================================================
// Observation-slimming helpers (T0). Pure string transforms, unit-tested in
// observation_slim_unittest.cc.
// =============================================================================

// Max bytes an element `href` may occupy in the observation before it is
// truncated (with a trailing `href_truncated` marker set by the caller).
// Standardized on the p90 of observed href lengths (~133) rounded down.
inline constexpr int kHrefMaxBytes = 120;

// Compress an element's destination `href` for the observation:
//   * same-origin as `page_url` → drop the scheme+host, keep path+query
//     (the model reads relative links fine, and 80% of links are same-origin);
//   * strip well-known tracking params (utm_*, fbclid, gclid, …);
//   * if still longer than kHrefMaxBytes, truncate on a UTF-8 boundary and
//     return `truncated=true` via the out-param.
// Returns the compressed href. `page_url` may be empty (e.g. no observed URL
// yet), in which case only tracking-strip + truncation apply. Deterministic
// and side-effect free — the dom_hash projection intentionally does NOT use
// this (it keeps the raw href), so slimming the emitted href does not perturb
// the stagnation fingerprint.
std::string CompressHref(std::string_view href,
                         std::string_view page_url,
                         bool* truncated);

// Decide whether the working-tab observation can be short-circuited to an
// `unchanged` stub instead of a full elements[] payload (T0-c). All inputs
// are values the caller already has:
//   * `enabled` — the guest opted into observation slimming (new-guest
//     handshake); false for old guests → never short-circuit.
//   * `stale` — the guest signalled its context was compacted / restored /
//     replayed since the last send, so the base full observation may be gone
//     from the model's context; true → must re-send full.
//   * `prev_doc` / `cur_doc` — the working tab's document_identifier last
//     time and now; a mismatch (navigation, cross-doc) forces full.
//   * `prev_hash` / `cur_hash` — the normalized-projection hash last sent and
//     now; equal means the interactive surface is byte-identical.
// Short-circuit only when enabled AND !stale AND same non-empty doc AND
// equal non-empty hashes.
bool ShouldShortCircuitObservation(bool enabled,
                                   bool stale,
                                   std::string_view prev_doc,
                                   std::string_view cur_doc,
                                   std::string_view prev_hash,
                                   std::string_view cur_hash);

// T0-c baseline bookkeeping — the "last full observation the model actually
// received" that `ShouldShortCircuitObservation` compares against. Pure
// (no browser types), unit-tested in observation_slim_unittest.cc.
//
// The invariant it exists to hold: **the baseline moves only when the
// elements[] payload ships to the model.** `BuildObservationDict` walks the
// page and can only *stage* the projection it produced; the caller that
// resolves the tool result decides what happened to those elements:
//   * they went out in the result (an action's post-observation, a
//     `read_page`/`find_text` called with `include_observation:true`) →
//     `Commit()` — the staged projection becomes the baseline;
//   * they were stripped to basics-only (default `read_page`/`find_text`)
//     or the observation was discarded (the late-navigation settle guard
//     re-observes and resolves with the second observation) →
//     `Discard()` — the baseline stays where the model's context is.
// Without that split, a stripped read would move the baseline to a surface
// the model never saw and a later same-document action could collapse to an
// `{unchanged}` stub while the model still acts on the older page.
class ObservationBaseline {
 public:
  ObservationBaseline();
  ObservationBaseline(const ObservationBaseline&) = delete;
  ObservationBaseline& operator=(const ObservationBaseline&) = delete;
  ~ObservationBaseline();

  // Pure decision against the COMMITTED baseline only — a staged projection
  // is invisible here until `Commit()`.
  bool ShouldShortCircuit(bool enabled,
                          std::string_view cur_doc,
                          std::string_view cur_hash) const;

  // Record the projection of a freshly-walked full observation. Replaces any
  // earlier staged value (an observation that was never resolved).
  void Stage(std::string document_id, std::string projection);

  // The staged elements shipped to the model: promote them to the baseline
  // and clear the stale flag (the base is now fresh in context). Returns
  // false (and changes nothing) when nothing was staged — e.g. the
  // observation was a short-circuit stub or had no working tab.
  bool Commit();

  // The staged elements did NOT ship — drop them; the baseline is untouched.
  void Discard();

  // Context mutation (compaction / restore / fork): the base observation may
  // be gone from the model's context. Forget the baseline and anything
  // staged; the next observation is sent full and re-establishes it.
  void MarkStale();

  bool stale() const { return stale_; }
  bool has_staged() const { return has_staged_; }
  const std::string& document_id() const { return document_id_; }
  const std::string& projection() const { return projection_; }

 private:
  std::string document_id_;
  std::string projection_;
  bool stale_ = false;
  bool has_staged_ = false;
  std::string staged_document_id_;
  std::string staged_projection_;
};

// Reduce an observation dict to basics-only (T0-b): remove the bulky
// `elements` / `screenshot` fields from every entry in `tabs`, leaving
// `tab_id` / `url` / `title` (and any `observation_error`). Used by the
// pure-query tools (read_page / find_text) whose answer is their
// page_text / matches payload — the full interactive-element list is
// redundant there unless the model explicitly asks for it. Returns the
// number of tabs whose elements were dropped.
int StripObservationToBasics(base::DictValue& observation);

}  // namespace nexus::browser_tool_internal

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_INTERNAL_H_
