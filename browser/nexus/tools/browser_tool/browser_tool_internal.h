#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_INTERNAL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_TOOL_INTERNAL_H_

// Internal-only header shared by browser_tool.cc and browser_tool_actions.cc;
// nothing outside nexus/tools/browser_tool/ should include it.

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

class BrowserWindowInterface;
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

// Reduce per-action results to one overall code via upstream's
// `actor::ExtractErrorResult`: the first non-OK code wins, else `kOk`.
actor::mojom::ActionResultCode OverallActorResultCode(
    const std::vector<actor::ActionResultWithLatencyInfo>& action_results);

// Map a typed `OnStopReason` to `actor::ActorTask::StoppedReason`.
// Exhaustive over the enum so any new reason added to OnStopReason
// forces an explicit decision here.
actor::ActorTask::StoppedReason ToActorStoppedReason(OnStopReason reason);

// Single source of truth for the APC AXRole → ARIA role-string mapping, used
// by both `IsActionable` and `ExtractRole`. Returns nullptr outside the
// allowlist of click / activate / select widget roles.
const char* RoleStringFromAXRole(apc::AXRole role);

// Most-recently-active Browser whose Profile matches `profile`, or
// nullptr.
BrowserWindowInterface* GetActiveBrowserForProfile(Profile* profile);

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

// Session-scoped variant: the session's most recent tab, else its first;
// nullopt when it owns none. Falls back to `GetActiveTabHandleId` when
// `browser_tool` is null or the flag is off.
std::optional<int32_t> GetSessionScopedActiveTabHandleId(
    ::nexus::BrowserTool* browser_tool);

// Resolves the explicit-or-active tab id; on miss rejects `done`
// (`no_active_tab` / `no_session_tab` / `tab_not_in_session`) and returns
// nullopt — the caller must return. With `browser_tool` and
// `kSessionScopedTabs`, an explicit id must carry the session's tag.
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

// Compress an element's `href`: same-origin → path+query; strip tracking
// params; truncate past kHrefMaxBytes on a UTF-8 boundary (`truncated`).
// Pure. The dom_hash projection keeps the raw href.
std::string CompressHref(std::string_view href,
                         std::string_view page_url,
                         bool* truncated);

// Whether the working-tab observation may collapse to an `unchanged` stub:
// only when `enabled`, not `stale`, the same non-empty document, and equal
// non-empty projection hashes.
bool ShouldShortCircuitObservation(bool enabled,
                                   bool stale,
                                   std::string_view prev_doc,
                                   std::string_view cur_doc,
                                   std::string_view prev_hash,
                                   std::string_view cur_hash);

// The last full observation the model received. Invariant: the baseline moves
// only when elements[] ship — `Commit()` then; `Discard()` when stripped or
// thrown away.
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
