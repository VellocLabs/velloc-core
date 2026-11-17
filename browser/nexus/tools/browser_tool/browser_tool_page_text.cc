#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/actor.mojom.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/frame_document.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

// =============================================================================
// File-local helpers used by `read_page` / `find_text` / `read_element`
// completions. Anonymous-namespaced because they're not referenced from
// browser_tool.cc.
// =============================================================================

// Trims `s` to end on a UTF-8 boundary, dropping a partial trailing sequence:
// base::Value CHECKs that strings are valid UTF-8.
void EnsureUtf8TailComplete(std::string& s) {
  if (s.empty()) {
    return;
  }
  // Walk back from the last byte until we find a leading byte (one
  // whose top two bits are NOT `10`). Continuation bytes match
  // `(b & 0xC0) == 0x80`.
  size_t i = s.size();
  while (i > 0) {
    --i;
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if ((b & 0xC0) == 0x80) {
      continue;  // continuation byte
    }
    // Determine the expected sequence length from the leading byte.
    int len = 1;
    if ((b & 0x80) == 0x00) {
      len = 1;  // ASCII
    } else if ((b & 0xE0) == 0xC0) {
      len = 2;
    } else if ((b & 0xF0) == 0xE0) {
      len = 3;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
    }
    const size_t tail_len = s.size() - i;
    if (tail_len == static_cast<size_t>(len)) {
      // Complete sequence — keep as-is.
      return;
    }
    // Sequence at `i` is incomplete. Drop it (and everything after).
    s.resize(i);
    return;
  }
  // No leading byte found at all (corrupt input) — clear it.
  s.clear();
}

// Joins the leaf text of every TEXT / PARAGRAPH / HEADING / LIST_ITEM
// node beneath `node` into `out`, separating consecutive text fragments
// with a single newline so the LLM can reason about paragraph breaks.
// Stops appending once `out` reaches `max_chars` and returns `false`
// to signal the walker to abort early.
bool AppendVisibleText(const apc::ContentNode& node,
                       int max_chars,
                       std::string& out) {
  if (static_cast<int>(out.size()) >= max_chars) {
    return false;
  }
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    apc::ContentAttributeType t = attrs.attribute_type();
    const bool is_text_carrier =
        t == apc::CONTENT_ATTRIBUTE_TEXT ||
        t == apc::CONTENT_ATTRIBUTE_PARAGRAPH ||
        t == apc::CONTENT_ATTRIBUTE_HEADING ||
        t == apc::CONTENT_ATTRIBUTE_LIST_ITEM;
    if (is_text_carrier && attrs.has_text_data()) {
      const std::string& s = attrs.text_data().text_content();
      if (!s.empty()) {
        if (!out.empty()) {
          out.push_back('\n');
        }
        const int budget = max_chars - static_cast<int>(out.size());
        if (budget <= 0) {
          return false;
        }
        if (static_cast<int>(s.size()) <= budget) {
          out.append(s);
        } else {
          // Byte-level slice may fall in the middle of a multi-byte
          // UTF-8 sequence (Chinese / Japanese / Korean / emoji etc.).
          // Append the prefix then walk back to the last full sequence
          // boundary so `out` stays valid UTF-8 — base::Value::Set
          // CHECKs UTF-8 validity and crashes the process otherwise.
          out.append(s, 0, static_cast<size_t>(budget));
          EnsureUtf8TailComplete(out);
          return false;
        }
      }
    }
  }
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (!AppendVisibleText(child, max_chars, out)) {
      return false;
    }
  }
  return true;
}

// Lower-cases ASCII characters in-place. The LLM-supplied query is
// expected to be plain ASCII for now; full Unicode case folding would
// require ICU, which we can wire later when a real CJK case-fold use
// case shows up.
std::string ToLowerAscii(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    out.push_back((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
  }
  return out;
}

// Recursive walker for `find_text`. For every node whose visible
// text contains `query_lower`, appends a `matches[]` entry with the text
// plus its nearest actionable ancestor's index / type / text (so
// the LLM can hand the ancestor's index straight to `click`).
//
// The ancestor index is the one the observation walk registered for that
// node (`NodeRegistry::IndexOf`); -1 when it has none.
struct FindTextWalker {
  // Owned (not a view) — the walker outlives the temporary returned by
  // ToLowerAscii(query) at construction time, so we copy the lowered
  // form into the struct.
  std::string query_lower;
  int max_results = 5;
  base::ListValue matches;
  raw_ptr<const NodeRegistry> registry = nullptr;
  int tab_id = 0;
  std::string main_document_id;
  // Document the node being visited belongs to.
  std::string document_id;
  // Stack of nearest actionable ancestor indexes.
  std::vector<int> ancestor_node_id_stack;
  std::vector<std::string> ancestor_type_stack;
  std::vector<std::string> ancestor_text_stack;
};

void FindTextWalk(const apc::ContentNode& node, FindTextWalker& w) {
  if (static_cast<int>(w.matches.size()) >= w.max_results) {
    return;
  }
  bool pushed_ancestor = false;
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    if (IsActionable(attrs) && attrs.has_common_ancestor_dom_node_id()) {
      const std::optional<int> index = w.registry->IndexOf(
          w.tab_id, w.document_id, attrs.common_ancestor_dom_node_id(),
          w.document_id == w.main_document_id);
      w.ancestor_node_id_stack.push_back(index.value_or(-1));
      w.ancestor_type_stack.push_back(
          ContentAttributeTypeName(attrs.attribute_type()));
      w.ancestor_text_stack.push_back(ExtractNodeText(attrs));
      pushed_ancestor = true;
    }

    // Match check on the current node's leaf text. We use
    // CONTENT_ATTRIBUTE_TEXT / PARAGRAPH / HEADING / LIST_ITEM /
    // ANCHOR / FORM_CONTROL because those are the carriers the LLM
    // would search for.
    if (attrs.has_text_data()) {
      const std::string& raw = attrs.text_data().text_content();
      if (!raw.empty()) {
        const std::string lowered = ToLowerAscii(raw);
        if (lowered.find(w.query_lower) != std::string::npos) {
          base::DictValue match;
          match.Set("text", raw);
          if (!w.ancestor_node_id_stack.empty() &&
              w.ancestor_node_id_stack.back() >= 0) {
            match.Set("clickable_ancestor_index",
                      w.ancestor_node_id_stack.back());
            match.Set("ancestor_type", w.ancestor_type_stack.back());
            if (!w.ancestor_text_stack.back().empty()) {
              match.Set("ancestor_text", w.ancestor_text_stack.back());
            }
          }
          w.matches.Append(std::move(match));
        }
      }
    }
  }
  const std::string parent_document_id = w.document_id;
  w.document_id = ChildDocumentId(node, parent_document_id);
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (static_cast<int>(w.matches.size()) >= w.max_results) {
      break;
    }
    FindTextWalk(child, w);
  }
  w.document_id = parent_document_id;
  if (pushed_ancestor) {
    w.ancestor_node_id_stack.pop_back();
    w.ancestor_type_stack.pop_back();
    w.ancestor_text_stack.pop_back();
  }
}

}  // namespace

void BrowserTool::RunReadPageOnUI(::nexus::generated::BrowserReadPageInput input,
                                  ToolCallback done) {
  // Phase 13: schema no longer exposes tab_id / scope / max_chars.
  // tab_id → always-active; scope/max_chars → fixed server-side defaults
  // (full-page walk capped at 4000 chars; the body sets `truncated` when
  // the cap is hit). The legacy two-scope split is dead-code today (the
  // actor framework's APC fetch is always full-page either way), so
  // collapsing to a single mode is a no-op behavior change.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "read_page", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  constexpr int kReadPageMaxChars = 4000;
  RunTabObservation(
      *resolved_tab_id, "read_page", std::move(done),
      base::BindOnce(&BrowserTool::OnReadPageObservationBuilt,
                     weak_ptr_factory_.GetWeakPtr(),
                     /*scope=*/std::string("full_page"),
                     /*max_chars=*/kReadPageMaxChars,
                     *resolved_tab_id,
                     /*include_observation=*/
                     input.include_observation.value_or(false)));
}

void BrowserTool::RunReadElementOnUI(::nexus::generated::BrowserReadElementInput input,
                                     ToolCallback done) {
  // Pure registry lookup. The node_registry already cached the
  // visible label / inner text from the most recent observation, so
  // there's no need to re-fetch the APC tree — the LLM uses this tool
  // for tiny snippets (button labels, list-item text) where the
  // registry's cached `text` is exactly what's wanted.
  //
  // Phase 13: dropped tab_id (use registry's home tab) and max_chars
  // (fixed server-side cap).
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "read_element");
  if (!entry) {
    return;  // LookupNode already rejected.
  }
  constexpr int kReadElementMaxChars = 2000;
  std::string text = entry->text;
  bool truncated = false;
  if (static_cast<int>(text.size()) > kReadElementMaxChars) {
    text.resize(static_cast<size_t>(kReadElementMaxChars));
    // Byte-level resize may bisect a multi-byte UTF-8 sequence on
    // pages with non-ASCII text — see EnsureUtf8TailComplete's docs.
    EnsureUtf8TailComplete(text);
    truncated = true;
  }
  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "read_element");
  result.Set("index", input.index);
  result.Set("type", entry->type);
  result.Set("element_text", std::move(text));
  result.Set("truncated", truncated);
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::RunFindTextOnUI(::nexus::generated::BrowserFindTextInput input,
                                  ToolCallback done) {
  // No `input.query.empty()` guard — the codegen-emitted Extract()
  // already rejects an empty required string with `invalid_args`
  // before this body runs (see find_text.gen.h).
  //
  // Phase 13: dropped tab_id (always active tab) and max_results
  // (fixed server-side cap).
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "find_text", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  constexpr int kFindTextMaxResults = 5;
  RunTabObservation(
      *resolved_tab_id, "find_text", std::move(done),
      base::BindOnce(&BrowserTool::OnFindTextObservationBuilt,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(input.query), kFindTextMaxResults,
                     /*include_observation=*/
                     input.include_observation.value_or(false)));
}

// =============================================================================
// Custom completions used by `read_page` + `find_text`. They walk the
// observation proto themselves to produce the LLM-facing payload
// instead of falling back to the generic OnActionsResultBuilt path.
// =============================================================================

void BrowserTool::OnReadPageObservationBuilt(
    std::string scope,
    int max_chars,
    int32_t requested_tab_id,
    bool include_observation,
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
  // 149 folded the standalone result_code / index_of_failed_action into
  // the per-action `action_results` vector; reduce it via upstream's
  // canonical helper (first non-OK wins). Observation-only paths
  // (read_page / find_text) dispatch no actions, so this is kOk unless the
  // framework reports an observation-time failure.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  // Node registry accumulates across observations — see comment in
  // OnActionsResultBuilt. BuildObservationDict below registers the
  // new observation's nodes; prior nodes that didn't reappear remain
  // in the registry and surface a clean stale error if used.

  base::DictValue result;
  result.Set("action", action_name);
  result.Set("scope", scope);
  if (!actor::IsOk(result_code)) {
    result.Set("ok", false);
    result.Set("error_code", static_cast<int>(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }
  if (!result_proto) {
    result.Set("ok", false);
    result.Set("result", "no_observation");
    std::move(done).Resolve(std::move(result));
    return;
  }
  // BuildObservationDict always runs (registry + agent_meta), but elements[] are
  // stripped to basics by default — read_page answers with page_text — unless
  // `include_observation:true` (then always full). A stripped result discards
  // the staged baseline.
  base::DictValue observation = BuildObservationDict(
      *result_proto, /*include_screenshot_image=*/false,
      /*force_full=*/include_observation);
  if (!observation.empty()) {
    if (include_observation) {
      observation_baseline_.Commit();
    } else {
      observation_baseline_.Discard();
      browser_tool_internal::StripObservationToBasics(observation);
    }
    result.Set("observation", std::move(observation));
  } else {
    observation_baseline_.Discard();
  }

  // Find the requested tab within the observation. The actor framework
  // can return multiple `TabObservation`s in `result_proto->tabs()`
  // (e.g. when it batches observations across recently-acted tabs),
  // and historically we picked the first one — which silently returned
  // the wrong tab's content when navigation had moved focus elsewhere.
  // Match against `requested_tab_id` first; fall back to the first tab
  // with content if the requested tab somehow isn't in the observation.
  const apc::TabObservation* match = nullptr;
  const apc::TabObservation* first_with_content = nullptr;
  for (const apc::TabObservation& tab_obs : result_proto->tabs()) {
    if (!tab_obs.has_annotated_page_content()) {
      continue;
    }
    if (!tab_obs.annotated_page_content().has_root_node()) {
      continue;
    }
    if (!first_with_content) {
      first_with_content = &tab_obs;
    }
    if (tab_obs.id() == requested_tab_id) {
      match = &tab_obs;
      break;
    }
  }

  std::string page_text;
  std::optional<int> picked_tab_id;
  const apc::TabObservation* selected = match ? match : first_with_content;
  if (selected) {
    const apc::AnnotatedPageContent& apc_data =
        selected->annotated_page_content();
    AppendVisibleText(apc_data.root_node(), max_chars, page_text);
    picked_tab_id = selected->id();
    if (apc_data.has_main_frame_data()) {
      if (!apc_data.main_frame_data().url().empty()) {
        result.Set("url", apc_data.main_frame_data().url());
      }
      if (!apc_data.main_frame_data().title().empty()) {
        result.Set("title", apc_data.main_frame_data().title());
      }
    }
    if (!match && first_with_content) {
      // Defensive: requested tab wasn't in the observation set.
      // Surface a warning so the model knows the content came from a
      // different tab and can react (e.g. activate the right tab)
      // instead of silently consuming the wrong page.
      result.Set(
          "tab_mismatch_warning",
          base::StrCat(
              {"requested tab ", base::NumberToString(requested_tab_id),
               " was not in the observation; returning tab ",
               base::NumberToString(picked_tab_id.value_or(-1)),
               " content instead. Consider calling activate_tab first."}));
    }
  }
  if (picked_tab_id.has_value()) {
    result.Set("tab_id", *picked_tab_id);
  }
  result.Set("page_text", page_text);
  result.Set("truncated", static_cast<int>(page_text.size()) >= max_chars);
  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::OnFindTextObservationBuilt(
    std::string query,
    int max_results,
    bool include_observation,
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
  // 149 folded the standalone result_code / index_of_failed_action into
  // the per-action `action_results` vector; reduce it via upstream's
  // canonical helper (first non-OK wins). Observation-only paths
  // (read_page / find_text) dispatch no actions, so this is kOk unless the
  // framework reports an observation-time failure.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  base::DictValue result;
  result.Set("action", action_name);
  result.Set("query", query);
  if (!actor::IsOk(result_code)) {
    result.Set("ok", false);
    result.Set("error_code", static_cast<int>(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }
  if (!result_proto) {
    result.Set("ok", false);
    result.Set("result", "no_observation");
    std::move(done).Resolve(std::move(result));
    return;
  }

  // Refresh the observation (also updates the node registry and
  // last_observation_* fields used by ToolSessionRegistry for agent_meta).
  // T0-b: default to basics-only — find_text's answer is its matches[]
  // (each already carries `clickable_ancestor_index`, enough to click
  // the result directly). `include_observation:true` returns the full
  // elements[] for a search-then-act-elsewhere flow — as an array, never
  // a T0-c stub (`force_full`). Baseline commit/discard mirrors read_page:
  // only shipped elements move the baseline.
  base::DictValue observation = BuildObservationDict(
      *result_proto, /*include_screenshot_image=*/false,
      /*force_full=*/include_observation);
  if (!observation.empty()) {
    if (include_observation) {
      observation_baseline_.Commit();
    } else {
      observation_baseline_.Discard();
      browser_tool_internal::StripObservationToBasics(observation);
    }
    result.Set("observation", std::move(observation));
  } else {
    observation_baseline_.Discard();
  }

  // Find walk. Limited to the first APC tree (the active observed
  // tab) since `find_text` is one-tab.
  FindTextWalker walker;
  walker.query_lower = ToLowerAscii(query);
  walker.max_results = max_results;
  for (const apc::TabObservation& tab_obs : result_proto->tabs()) {
    if (!tab_obs.has_annotated_page_content()) {
      continue;
    }
    const apc::AnnotatedPageContent& apc_data = tab_obs.annotated_page_content();
    if (apc_data.has_root_node()) {
      walker.registry = &node_registry_;
      walker.tab_id =
          apc_data.has_tab_id() ? apc_data.tab_id() : tab_obs.id();
      if (apc_data.has_main_frame_data() &&
          apc_data.main_frame_data().has_document_identifier()) {
        walker.main_document_id =
            apc_data.main_frame_data().document_identifier().serialized_token();
      }
      walker.document_id = walker.main_document_id;
      FindTextWalk(apc_data.root_node(), walker);
    }
    break;
  }
  result.Set("matches", std::move(walker.matches));
  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

}  // namespace nexus
