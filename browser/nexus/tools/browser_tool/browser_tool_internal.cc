#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "url/gurl.h"
#include "chrome/browser/glic/actor/glic_actor_policy_checker.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor/action_result.h"
#include "components/sessions/core/session_id.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "custom_browser/browser/nexus/session/nexus_session_features.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"

namespace nexus::browser_tool_internal {

// =============================================================================
// PausableTimeout — out-of-line ctor / dtor / methods. Chromium-style
// requires complex bodies (any class with non-trivial members) to be
// defined in the .cc, even if the body is `= default;`.
// =============================================================================

PausableTimeout::PausableTimeout(base::TimeDelta budget,
                                 std::string name,
                                 base::OnceClosure on_timeout)
    : remaining_(budget),
      name_(std::move(name)),
      on_timeout_(std::move(on_timeout)) {}

PausableTimeout::~PausableTimeout() = default;

void PausableTimeout::Start(bool paused) {
  if (!remaining_.is_positive()) {
    Stop();
    return;
  }
  paused_ = paused;
  if (!paused_) {
    ArmTimer();
  }
}

void PausableTimeout::Pause() {
  if (settled_ || paused_) {
    return;
  }
  if (timer_.IsRunning()) {
    remaining_ -= base::TimeTicks::Now() - started_at_;
    timer_.Stop();
  }
  paused_ = true;
}

void PausableTimeout::Resume() {
  if (settled_ || !paused_) {
    return;
  }
  paused_ = false;
  if (!remaining_.is_positive()) {
    Fire();
    return;
  }
  ArmTimer();
}

void PausableTimeout::Stop() {
  if (settled_) {
    return;
  }
  settled_ = true;
  timer_.Stop();
  on_timeout_.Reset();
}

void PausableTimeout::ArmTimer() {
  started_at_ = base::TimeTicks::Now();
  timer_.Start(FROM_HERE, remaining_, this, &PausableTimeout::Fire);
}

void PausableTimeout::Fire() {
  scoped_refptr<PausableTimeout> keep_alive(this);
  if (settled_) {
    return;
  }
  settled_ = true;
  timer_.Stop();
  LOG(WARNING) << "[BrowserTool] " << name_ << " exceeded timeout";
  if (on_timeout_) {
    std::move(on_timeout_).Run();
  }
}

// =============================================================================
// PausableToolCallbackHolder — same out-of-line treatment.
// =============================================================================

PausableToolCallbackHolder::PausableToolCallbackHolder(
    ::nexus::ToolCallback inner,
    std::string action_name)
    : inner_(std::move(inner)), action_name_(std::move(action_name)) {}

PausableToolCallbackHolder::~PausableToolCallbackHolder() = default;

void PausableToolCallbackHolder::SetTimeout(
    scoped_refptr<PausableTimeout> timeout) {
  timeout_ = std::move(timeout);
}

void PausableToolCallbackHolder::Resolve(base::Value value) {
  if (timeout_) {
    timeout_->Stop();
  }
  if (inner_.fired()) {
    return;
  }
  std::move(inner_).Resolve(std::move(value));
}

void PausableToolCallbackHolder::Reject(std::string code,
                                        base::DictValue params) {
  if (timeout_) {
    timeout_->Stop();
  }
  if (inner_.fired()) {
    return;
  }
  std::move(inner_).Reject(std::move(code), std::move(params));
}

void PausableToolCallbackHolder::OnTimeout() {
  if (inner_.fired()) {
    return;
  }
  std::move(inner_).Reject("tool_action_timeout", ActionParams(action_name_));
}

const actor::EnterprisePolicyChecker* GetPolicyChecker(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto* service =
      glic::GlicKeyedServiceFactory::GetGlicKeyedService(profile);
  if (!service) {
    return nullptr;
  }
  return &service->actor_policy_checker();
}

base::DictValue ActionParams(std::string_view action,
                             std::string_view phase) {
  base::DictValue params;
  if (!action.empty()) {
    params.Set("action", action);
  }
  if (!phase.empty()) {
    params.Set("phase", phase);
  }
  return params;
}

base::DictValue PhaseParams(std::string_view phase) {
  base::DictValue params;
  params.Set("phase", phase);
  return params;
}

base::DictValue ReasonParams(std::string_view reason) {
  base::DictValue params;
  params.Set("reason", reason);
  return params;
}

actor::mojom::ActionResultCode OverallActorResultCode(
    const std::vector<actor::ActionResultWithLatencyInfo>& action_results) {
  // Delegate to upstream's canonical reducer so Nexus stays in lockstep
  // with whatever the actor framework considers the "overall" result.
  // `ExtractErrorResult` only writes when it finds a non-OK entry, so seed
  // with kOk for the all-success case. The index out-param is unused here
  // (Nexus surfaces the failing element through the observation, not an
  // index), but the API requires it.
  actor::mojom::ActionResultCode code = actor::mojom::ActionResultCode::kOk;
  std::optional<size_t> index_of_failed_action;
  actor::ExtractErrorResult(action_results, &code, index_of_failed_action);
  return code;
}

actor::ActorTask::StoppedReason ToActorStoppedReason(OnStopReason reason) {
  switch (reason) {
    case OnStopReason::kCompleted:
      return actor::ActorTask::StoppedReason::kTaskComplete;
    case OnStopReason::kUserStopped:
      return actor::ActorTask::StoppedReason::kStoppedByUser;
    case OnStopReason::kSessionDeleted:
      // User-initiated teardown — closer to "user stopped" than
      // "shutdown" from the actor task's perspective. The session's
      // tabs are about to be closed by ~BrowserTool, but that's
      // orthogonal to the actor task's stop reason.
      return actor::ActorTask::StoppedReason::kStoppedByUser;
    case OnStopReason::kDisconnected:
    case OnStopReason::kShutdown:
    case OnStopReason::kDestructor:
      return actor::ActorTask::StoppedReason::kShutdown;
  }
  return actor::ActorTask::StoppedReason::kStoppedByUser;
}

Browser* GetActiveBrowserForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  // custom-browser: Chromium 149's FindLastActiveWithProfile returns
  // BrowserWindowInterface*.
  auto* bwi = chrome::FindLastActiveWithProfile(profile);
  return bwi ? bwi->GetBrowserForMigrationOnly() : nullptr;
}

std::optional<int32_t> GetActiveWindowSessionId(Profile* profile) {
  Browser* browser = GetActiveBrowserForProfile(profile);
  if (!browser) {
    return std::nullopt;
  }
  return browser->session_id().id();
}

std::optional<int32_t> ResolveCreateTabWindowSessionId(Profile* profile) {
  if (!profile) {
    return std::nullopt;
  }

  // Default (pref off): pin agent tabs to the kNexus window. "Active
  // window" (the old behavior) leaks tabs into whatever normal window the
  // user focused last — and the guest's per-session webview_area can only
  // display tabs hosted in the Nexus window's own tab strip
  // (main-contents-view shows that window's active tab). Fall back to the
  // active window only when no Nexus window exists (teardown races; while
  // a guest is alive the panel's window exists by construction).
  if (!::nexus::ShouldHostAgentTabsInSeparateWindow(profile)) {
    Browser* nexus_window =
        custom_browser::window_util::FindNexusWindow(profile);
    if (nexus_window) {
      return nexus_window->session_id().id();
    }
    return GetActiveWindowSessionId(profile);
  }

  // Pref on: route agent tabs into a separate NORMAL (non-kNexus) browsing
  // window so they stay out of the agent chat window. Resolved through
  // custom_browser::window_util — the canonical owner of the kNexus-vs-normal
  // window-mode logic — rather than a hand-rolled Browser::Create + tracking
  // map, so the window it returns is a real normal CustomBrowserView window.
  // The same window is reused across all sessions for this profile (per-
  // session tab groups keep them visually distinct); it's created on demand.
  Browser* window =
      custom_browser::window_util::FindOrCreateNormalWindow(profile);
  if (!window) {
    // Creation failed — fall back to the active window so the create still
    // succeeds rather than rejecting the tool call.
    return GetActiveWindowSessionId(profile);
  }
  return window->session_id().id();
}

std::optional<int32_t> GetActiveTabHandleId(Profile* profile) {
  Browser* browser = GetActiveBrowserForProfile(profile);
  if (!browser) {
    return std::nullopt;
  }
  TabStripModel* tab_strip = browser->tab_strip_model();
  if (!tab_strip) {
    return std::nullopt;
  }
  tabs::TabInterface* active = tab_strip->GetActiveTab();
  if (!active) {
    return std::nullopt;
  }
  return active->GetHandle().raw_value();
}

std::optional<int32_t> GetSessionScopedActiveTabHandleId(
    ::nexus::BrowserTool* browser_tool) {
  if (!browser_tool || !::nexus::features::IsSessionScopedTabsEnabled() ||
      browser_tool->GetSessionKey().empty()) {
    return GetActiveTabHandleId(browser_tool ? browser_tool->GetProfile()
                                              : nullptr);
  }

  // Three-level resolution, in priority order:
  //
  //   1. Chrome's actually-active tab IF it carries this session's tag.
  //      Aligns with the system-prompt language "Most tools act on the
  //      currently-active tab" — whatever the LLM (or the user) brought
  //      to the foreground wins.
  //
  //   2. `BrowserTool::LastWorkTab()` — the most recent tab a tool
  //      actually dispatched against, validated still-alive and still
  //      session-tagged. Answers "the LLM was working on X; now it's
  //      calling a tab-less tool, default to X" when the user's
  //      foreground tab is outside the session. Set only by
  //      `RunActorAction` / `RunTabObservation`, NOT by tab
  //      creation/activation — so it tracks real work, not focus
  //      cycles. Renamed from the earlier `last_active_tab_` to make
  //      the semantic precise.
  //
  //   3. First session-owned tab from `CollectSessionTabs()` in
  //      browser-window-activation order. Deterministic fallback when
  //      neither (1) nor (2) applies (e.g. first tool call of a fresh
  //      session before any work has dispatched).
  Profile* profile = browser_tool->GetProfile();
  if (std::optional<int32_t> active = GetActiveTabHandleId(profile);
      active.has_value()) {
    tabs::TabInterface* tab = tabs::TabHandle(*active).Get();
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    const std::string* tag =
        ::nexus::NexusSessionTabTag::SessionKeyFor(contents);
    if (tag && *tag == browser_tool->GetSessionKey()) {
      return *active;
    }
  }
  if (std::optional<tabs::TabHandle> last_work = browser_tool->LastWorkTab();
      last_work.has_value()) {
    return last_work->raw_value();
  }
  std::vector<tabs::TabHandle> tabs = browser_tool->CollectSessionTabs();
  if (tabs.empty()) {
    return std::nullopt;
  }
  return tabs.front().raw_value();
}

std::optional<int32_t> ResolveTabIdOrReject(
    std::optional<int> explicit_tab_id,
    Profile* profile,
    std::string_view tool_name,
    ::nexus::ToolCallback& done,
    ::nexus::BrowserTool* browser_tool) {
  const bool session_scope =
      browser_tool && ::nexus::features::IsSessionScopedTabsEnabled() &&
      !browser_tool->GetSessionKey().empty();

  if (explicit_tab_id.has_value()) {
    if (session_scope) {
      tabs::TabInterface* tab =
          tabs::TabHandle(*explicit_tab_id).Get();
      content::WebContents* contents = tab ? tab->GetContents() : nullptr;
      const std::string* tag =
          ::nexus::NexusSessionTabTag::SessionKeyFor(contents);
      if (!tag || *tag != browser_tool->GetSessionKey()) {
        {
      base::DictValue reject_params;
      reject_params.Set("tool", tool_name);
      reject_params.Set("tab_id", *explicit_tab_id);
      std::move(done).Reject("tab_not_in_session", std::move(reject_params));
    }
        return std::nullopt;
      }
    }
    return *explicit_tab_id;
  }

  if (session_scope) {
    std::optional<int32_t> resolved =
        GetSessionScopedActiveTabHandleId(browser_tool);
    if (!resolved.has_value()) {
      {
      base::DictValue reject_params;
      reject_params.Set("tool", tool_name);
      std::move(done).Reject("no_session_tab", std::move(reject_params));
    }
      return std::nullopt;
    }
    return *resolved;
  }

  std::optional<int32_t> active = GetActiveTabHandleId(profile);
  if (!active.has_value()) {
    {
    base::DictValue reject_params = ReasonParams("no_active");
    reject_params.Set("tool", tool_name);
    std::move(done).Reject("no_active_tab", std::move(reject_params));
  }
    return std::nullopt;
  }
  return *active;
}

void MaybePrependActivateTab(apc::Actions* actions,
                             int32_t target_tab_id,
                             Profile* profile) {
  std::optional<int32_t> active = GetActiveTabHandleId(profile);
  if (!active.has_value() || *active == target_tab_id) {
    return;
  }
  apc::ActivateTabAction* activate =
      actions->add_actions()->mutable_activate_tab();
  activate->set_tab_id(target_tab_id);
}

const char* ContentAttributeTypeName(apc::ContentAttributeType t) {
  switch (t) {
    case apc::CONTENT_ATTRIBUTE_ANCHOR:
      return "link";
    case apc::CONTENT_ATTRIBUTE_FORM_CONTROL:
      return "form_control";
    case apc::CONTENT_ATTRIBUTE_FORM:
      return "form";
    case apc::CONTENT_ATTRIBUTE_HEADING:
      return "heading";
    case apc::CONTENT_ATTRIBUTE_PARAGRAPH:
      return "paragraph";
    case apc::CONTENT_ATTRIBUTE_TEXT:
      return "text";
    case apc::CONTENT_ATTRIBUTE_IMAGE:
      return "image";
    case apc::CONTENT_ATTRIBUTE_TABLE:
      return "table";
    case apc::CONTENT_ATTRIBUTE_LIST_ITEM:
      return "list_item";
    case apc::CONTENT_ATTRIBUTE_IFRAME:
      return "iframe";
    case apc::CONTENT_ATTRIBUTE_VIDEO:
      return "video";
    default:
      return "element";
  }
}

std::string ExtractNodeText(const apc::ContentAttributes& attrs) {
  if (attrs.has_label() && !attrs.label().empty()) {
    return attrs.label();
  }
  if (attrs.has_text_data() && !attrs.text_data().text_content().empty()) {
    return attrs.text_data().text_content();
  }
  return std::string();
}

// Single source of truth for the APC AXRole → ARIA role-string mapping.
// See the header comment in browser_tool_internal.h for the policy.
//
// Two batches:
//   1. Pre-existing (option / menuitem / tab / treeitem family). The
//      "list-style picker" widgets that surface as ARIA-only nodes —
//      Healthgrades' insurance autocomplete and similar React widgets.
//   2. Direct-control widget roles (button / link / checkbox / radio /
//      switch). The W3C ARIA "widget roles" subset that imply a single
//      click-to-activate interaction with no required value-entry /
//      arrow-key / popup semantics. Catches modern React surfaces
//      (Radix, Headless UI, Material) that render their primitives as
//      `<div role="button">` etc. without native HTML interactivity.
//      AX_ROLE_TOGGLEBUTTON returns "button" too — the model already
//      knows how to click a button, and the toggle-state semantics
//      (`aria-pressed`) are a separate exposure follow-up.
//
// Intentionally NOT included (would mislead the model — needs richer
// metadata than just a role string): SLIDER, SPINBUTTON, COMBOBOX*,
// SEARCHBOX, DISCLOSURETRIANGLE (expanded-state), MENUBUTTON / POPUPBUTTON
// (popup management). Those are tracked as a separate follow-up.
const char* RoleStringFromAXRole(apc::AXRole role) {
  switch (role) {
    // Batch 1 — list-style picker family (shipped earlier; found on a
    // healthcare-directory site).
    case apc::AX_ROLE_LISTBOXOPTION:
    case apc::AX_ROLE_MENULISTOPTION:
      return "option";
    case apc::AX_ROLE_MENUITEM:
      return "menuitem";
    // NOTE: `MENUITEMCHECKBOX` and `MENUITEMRADIO` are kept here as
    // defensive forward-compat, but empirically Blink normalizes
    // `<div role="menuitemcheckbox" aria-checked="…">` (and the radio
    // variant) to a synthesized FORM_CONTROL with
    // `input_type:"checkbox"` / `"radio"`, and APC's `aria_role` field
    // collapses to `CHECKBOX` / `RADIOBUTTON` — same shape as
    // `AX_ROLE_SWITCH` below. The model sees role:"checkbox" plus the
    // visible label text, which is sufficient to actuate the control.
    // These cases fire only if Blink ever preserves the distinguishing
    // role (e.g. without `aria-checked`, though that's semantically
    // invalid per W3C ARIA).
    case apc::AX_ROLE_MENUITEMCHECKBOX:
      return "menuitemcheckbox";
    case apc::AX_ROLE_MENUITEMRADIO:
      return "menuitemradio";
    case apc::AX_ROLE_TAB:
      return "tab";
    case apc::AX_ROLE_TREEITEM:
      return "treeitem";
    // Batch 2 — direct-control widget roles (single click-to-activate).
    case apc::AX_ROLE_BUTTON:
    case apc::AX_ROLE_TOGGLEBUTTON:
      return "button";
    case apc::AX_ROLE_LINK:
      return "link";
    case apc::AX_ROLE_CHECKBOX:
      return "checkbox";
    case apc::AX_ROLE_RADIOBUTTON:
      return "radio";
    // NOTE: `AX_ROLE_SWITCH` is intentionally NOT mapped here. Blink
    // normalizes `<div role="switch">` (and `<input type="checkbox"
    // switch>`) to `AX_ROLE_CHECKBOX` in the proto's `aria_role`
    // field — the SWITCH enum value never appears for switches we
    // actually need to recognize. APC additionally synthesizes a
    // FORM_CONTROL representation with `input_type:"checkbox"` for
    // these elements, so the model already sees a usable signal
    // (role:"checkbox" + text label). Adding a SWITCH case here
    // would be dead code that misleads readers.
    default:
      return nullptr;
  }
}

bool IsActionable(const apc::ContentAttributes& attrs) {
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR ||
      attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL) {
    return true;
  }
  if (attrs.has_interaction_info()) {
    const auto& ii = attrs.interaction_info();
    if (ii.is_clickable() || ii.is_focusable()) {
      return true;
    }
  }
  // ARIA-only interactive roles. React/Vue autocompletes routinely render
  // suggestions as <li role="option"> (and menus as <li role="menuitem">,
  // tab strips as <div role="tab">, etc.) with no native click handler and
  // no FORM_CONTROL/ANCHOR attribute_type, so they fall through the two
  // checks above and never reach elements[]. APC's `aria_role` (proto field
  // 26 on ContentAttributes, populated in ACTIONABLE_ELEMENTS mode) carries
  // the resolved AXRole — defer to the shared `RoleStringFromAXRole`
  // allowlist so this gate and the role-string emission in ExtractRole
  // can't drift apart.
  if (attrs.has_aria_role() &&
      RoleStringFromAXRole(attrs.aria_role()) != nullptr) {
    return true;
  }
  return false;
}

std::string TruncateUtf8Snippet(std::string s, int max_bytes) {
  if (static_cast<int>(s.size()) <= max_bytes) {
    return s;
  }
  s.resize(static_cast<size_t>(max_bytes));
  size_t i = s.size();
  while (i > 0) {
    --i;
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if ((b & 0x80) == 0) {
      // Single-byte ASCII: complete, keep [0, i+1].
      s.resize(i + 1);
      return s;
    }
    if ((b & 0xC0) == 0xC0) {
      // Leading byte of a multi-byte sequence. Compute its expected
      // length and drop the partial sequence if `s.size()-i` is short.
      const size_t expected = (b & 0xE0) == 0xC0   ? 2
                              : (b & 0xF0) == 0xE0 ? 3
                              : (b & 0xF8) == 0xF0 ? 4
                                                   : 1;
      if (s.size() - i >= expected) {
        return s;
      }
      s.resize(i);
      return s;
    }
    // Continuation byte (0b10xxxxxx) — keep walking back.
  }
  s.clear();
  return s;
}

namespace {

// Query params that are pure tracking noise — dropped from observation
// hrefs. Matched case-insensitively on the key; `utm_` is a prefix match,
// the rest are exact. Deliberately conservative: only params that never
// affect where a link goes for the agent's purposes.
bool IsTrackingParam(std::string_view key) {
  std::string lower = base::ToLowerASCII(key);
  if (base::StartsWith(lower, "utm_")) {
    return true;
  }
  static constexpr std::array<std::string_view, 8> kExact = {
      "fbclid",   "gclid",  "dclid",     "gclsrc",
      "msclkid",  "mc_eid", "mc_cid",    "igshid"};
  for (std::string_view k : kExact) {
    if (lower == k) {
      return true;
    }
  }
  return false;
}

// Rebuild a query string with tracking params removed. Returns the
// filtered query (no leading '?'), preserving original order and the
// original key=value spelling of the params that survive.
std::string StripTrackingParams(std::string_view query) {
  std::vector<std::string_view> kept;
  for (std::string_view pair : base::SplitStringPiece(
           query, "&", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t eq = pair.find('=');
    const std::string_view key = pair.substr(0, eq);
    if (!IsTrackingParam(key)) {
      kept.push_back(pair);
    }
  }
  return base::JoinString(kept, "&");
}

}  // namespace

std::string CompressHref(std::string_view href,
                         std::string_view page_url,
                         bool* truncated) {
  if (truncated) {
    *truncated = false;
  }
  GURL url(href);
  std::string out;
  if (url.is_valid() && url.SchemeIsHTTPOrHTTPS()) {
    // Strip tracking params from the query. Rebuild via ReplaceComponents
    // rather than by hand so scheme/host/PORT/path/fragment survive
    // untouched (hand-concatenating scheme://host/path silently drops a
    // non-default port).
    const std::string query =
        url.has_query() ? StripTrackingParams(url.query()) : std::string();
    GURL::Replacements repl;
    if (query.empty()) {
      repl.ClearQuery();
    } else {
      repl.SetQueryStr(query);
    }
    const GURL rebuilt = url.ReplaceComponents(repl);

    GURL page(page_url);
    const bool same_origin =
        page.is_valid() && url.SchemeIs(page.scheme()) &&
        url.host() == page.host() &&
        url.EffectiveIntPort() == page.EffectiveIntPort();
    if (same_origin) {
      // Drop scheme+host (same as the page): path[?query], then fragment.
      out = rebuilt.PathForRequest();  // path + "?" + query, no fragment
      if (rebuilt.has_ref() && !rebuilt.ref().empty()) {
        base::StrAppend(&out, {"#", rebuilt.ref()});
      }
    } else {
      // Cross-origin: keep the full absolute URL (port and all), only the
      // tracking params removed.
      out = rebuilt.spec();
    }
  } else {
    // Non-http(s) or unparseable — pass through untouched (mailto:,
    // javascript:, relative fragments the model may still need verbatim).
    out = std::string(href);
  }

  if (static_cast<int>(out.size()) > kHrefMaxBytes) {
    out = TruncateUtf8Snippet(std::move(out), kHrefMaxBytes);
    if (truncated) {
      *truncated = true;
    }
  }
  return out;
}

bool ShouldShortCircuitObservation(bool enabled,
                                   bool stale,
                                   std::string_view prev_doc,
                                   std::string_view cur_doc,
                                   std::string_view prev_hash,
                                   std::string_view cur_hash) {
  if (!enabled || stale) {
    return false;
  }
  if (prev_doc.empty() || cur_doc.empty() || prev_doc != cur_doc) {
    return false;
  }
  if (prev_hash.empty() || cur_hash.empty() || prev_hash != cur_hash) {
    return false;
  }
  return true;
}

ObservationBaseline::ObservationBaseline() = default;
ObservationBaseline::~ObservationBaseline() = default;

bool ObservationBaseline::ShouldShortCircuit(bool enabled,
                                             std::string_view cur_doc,
                                             std::string_view cur_hash) const {
  return ShouldShortCircuitObservation(enabled, stale_, document_id_, cur_doc,
                                       projection_, cur_hash);
}

void ObservationBaseline::Stage(std::string document_id,
                                std::string projection) {
  staged_document_id_ = std::move(document_id);
  staged_projection_ = std::move(projection);
  has_staged_ = true;
}

bool ObservationBaseline::Commit() {
  if (!has_staged_) {
    return false;
  }
  document_id_ = std::move(staged_document_id_);
  projection_ = std::move(staged_projection_);
  staged_document_id_.clear();
  staged_projection_.clear();
  has_staged_ = false;
  stale_ = false;
  return true;
}

void ObservationBaseline::Discard() {
  staged_document_id_.clear();
  staged_projection_.clear();
  has_staged_ = false;
}

void ObservationBaseline::MarkStale() {
  stale_ = true;
  document_id_.clear();
  projection_.clear();
  Discard();
}

int StripObservationToBasics(base::DictValue& observation) {
  base::ListValue* tabs = observation.FindList("tabs");
  if (!tabs) {
    return 0;
  }
  int stripped = 0;
  for (base::Value& tab_value : *tabs) {
    base::DictValue* tab = tab_value.GetIfDict();
    if (!tab) {
      continue;
    }
    // `Remove` returns whether the key was present — count a tab as
    // stripped when it actually carried an elements[] payload.
    const bool had_elements = tab->Remove("elements");
    tab->Remove("screenshot");
    if (had_elements) {
      ++stripped;
    }
  }
  return stripped;
}

}  // namespace nexus::browser_tool_internal
