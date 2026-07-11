#include "custom_browser/browser/nexus/tools/nexus_tool_signature.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <vector>

#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "crypto/hash.h"
#include "url/gurl.h"

namespace nexus {

// =============================================================================
// AgentToolMeta — out-of-line ctor / copy / move / dtor required by
// chromium-style on structs with multiple non-trivial members.
// =============================================================================

AgentToolMeta::AgentToolMeta() = default;
AgentToolMeta::AgentToolMeta(const AgentToolMeta&) = default;
AgentToolMeta::AgentToolMeta(AgentToolMeta&&) noexcept = default;
AgentToolMeta& AgentToolMeta::operator=(const AgentToolMeta&) = default;
AgentToolMeta& AgentToolMeta::operator=(AgentToolMeta&&) noexcept = default;
AgentToolMeta::~AgentToolMeta() = default;

namespace {

// SHA-256 hex (lowercase) of the input string. Uses //crypto.
std::string Sha256Hex(std::string_view input) {
  std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(input);
  return base::ToLowerASCII(base::HexEncode(digest));
}

// Lowercase, strip non-word, split on whitespace, dedupe and sort.
// Matches the Browser-Use search-query normalization rules so
// "OpenAI Pricing" and "pricing openai" produce identical signatures.
std::string CanonicalizeSearchTokens(std::string_view query) {
  std::string lowered;
  lowered.reserve(query.size());
  for (char c : query) {
    if (base::IsAsciiAlpha(c) || base::IsAsciiDigit(c)) {
      lowered.push_back(base::ToLowerASCII(c));
    } else {
      lowered.push_back(' ');
    }
  }
  std::vector<std::string> tokens =
      base::SplitString(lowered, " ",
                        base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::set<std::string> dedup(tokens.begin(), tokens.end());
  std::vector<std::string> sorted(dedup.begin(), dedup.end());
  std::sort(sorted.begin(), sorted.end());
  return base::JoinString(sorted, "|");
}

std::string StripUrlFragment(std::string_view url) {
  GURL g{url};
  if (!g.is_valid()) {
    return std::string(url);
  }
  GURL::Replacements rep;
  rep.ClearRef();
  return g.ReplaceComponents(rep).possibly_invalid_spec();
}

std::string StableJsonWithoutNulls(const base::DictValue& args) {
  // Walk the dict in sorted key order; skip null values. Output is a
  // stable canonical JSON string suitable for hashing.
  std::vector<std::string> keys;
  keys.reserve(args.size());
  for (const auto entry : args) {
    keys.emplace_back(entry.first);
  }
  std::sort(keys.begin(), keys.end());

  base::DictValue clean;
  for (const std::string& key : keys) {
    const base::Value* v = args.Find(key);
    if (!v || v->is_none()) continue;
    clean.Set(key, v->Clone());
  }
  std::string out;
  base::JSONWriter::Write(clean, &out);
  return out;
}

std::optional<int> FindActionIndex(const base::DictValue& action) {
  return action.FindInt("index");
}

std::string CanonicalizeBrowserAction(const base::DictValue& action) {
  const std::string* type = action.FindString("type");
  if (!type || type->empty()) {
    return "unknown|{}";
  }

  if (*type == "wait" || *type == "done" || *type == "back") {
    return std::string();
  }

  if (*type == "search") {
    const std::string* engine = action.FindString("engine");
    const std::string* query = action.FindString("query");
    return base::StrCat({"search|",
                         engine ? *engine : "google", "|",
                         CanonicalizeSearchTokens(query ? *query : "")});
  }

  if (*type == "click" || *type == "select" || *type == "hover" ||
      *type == "read_element") {
    std::optional<int> index = FindActionIndex(action);
    return base::StrCat({"click|",
                         index ? base::NumberToString(*index) : "?"});
  }

  if (*type == "input") {
    std::optional<int> index = FindActionIndex(action);
    const std::string* text = action.FindString("text");
    std::string normalized;
    if (text) {
      normalized = base::ToLowerASCII(
          std::string(base::TrimWhitespaceASCII(*text, base::TRIM_ALL)));
    }
    return base::StrCat({"input|",
                         index ? base::NumberToString(*index) : "?", "|",
                         normalized});
  }

  if (*type == "navigate" || *type == "create_tab") {
    const std::string* url = action.FindString("url");
    return base::StrCat({"navigate|", url ? StripUrlFragment(*url) : ""});
  }

  if (*type == "scroll" || *type == "scroll_to") {
    const std::string* direction = action.FindString("direction");
    std::optional<int> index = FindActionIndex(action);
    return base::StrCat({"scroll|",
                         direction ? *direction : "down", "|",
                         index ? base::NumberToString(*index) : "?"});
  }

  if (*type == "drag_and_drop") {
    std::optional<int> from_id = action.FindInt("from_index");
    std::optional<int> to_id = action.FindInt("to_index");
    return base::StrCat({"drag|",
                         from_id ? base::NumberToString(*from_id) : "?",
                         "|",
                         to_id ? base::NumberToString(*to_id) : "?"});
  }

  return base::StrCat({*type, "|", StableJsonWithoutNulls(action)});
}

std::vector<std::string> CanonicalizeBrowserActions(
    const base::DictValue& args) {
  const base::ListValue* actions = args.FindList("actions");
  if (!actions || actions->empty()) {
    return {};
  }

  std::vector<std::string> parts;
  parts.reserve(actions->size());
  for (const base::Value& item : *actions) {
    if (!item.is_dict()) {
      continue;
    }
    std::string canonical = CanonicalizeBrowserAction(item.GetDict());
    if (!canonical.empty()) {
      parts.push_back(std::move(canonical));
    }
  }
  return parts;
}

}  // namespace

bool IsSequenceTerminator(std::string_view tool_name) {
  return tool_name == "browser_actions";
}

std::vector<std::string> ComputeActionHashes(std::string_view tool_name,
                                             const base::DictValue& args) {
  if (tool_name != "browser_actions") {
    return {};
  }

  std::vector<std::string> hashes;
  for (const std::string& canonical : CanonicalizeBrowserActions(args)) {
    std::string digest = Sha256Hex(canonical);
    hashes.push_back(digest.substr(0, 12));
  }
  return hashes;
}

std::string ComputeDomHash(std::string_view normalized_projection) {
  if (normalized_projection.empty()) {
    return std::string();
  }
  std::string digest = Sha256Hex(normalized_projection);
  return digest.substr(0, 16);
}

base::Value WrapResultWithAgentMeta(base::Value observation,
                                    const AgentToolMeta& meta) {
  base::DictValue out;
  out.Set("data", std::move(observation));

  base::DictValue agent_meta;
  if (!meta.action_hashes.empty()) {
    base::ListValue action_hashes;
    for (const std::string& hash : meta.action_hashes) {
      action_hashes.Append(hash);
    }
    agent_meta.Set("action_hashes", std::move(action_hashes));
  }
  if (!meta.page_dom_hash.empty()) {
    base::DictValue fp;
    fp.Set("url", meta.page_url);
    fp.Set("interactive_count", meta.page_interactive_count);
    fp.Set("dom_hash", meta.page_dom_hash);
    agent_meta.Set("page_fingerprint", std::move(fp));
  }
  if (!meta.post_url.empty()) {
    agent_meta.Set("post_url", meta.post_url);
  }
  if (meta.post_focus_target_present) {
    if (meta.post_focus_target_id.empty()) {
      agent_meta.Set("post_focus_target_id", base::Value());
    } else {
      agent_meta.Set("post_focus_target_id", meta.post_focus_target_id);
    }
  }
  if (meta.terminates_sequence) {
    agent_meta.Set("terminates_sequence", true);
  }

  // Phase 13.1: ALWAYS emit `agent_meta`, even when empty. The JS-side
  // unwrapper (`hostedTools.ts::unwrapData`) keys envelope detection
  // off the presence of this field:
  //
  //     if ('agent_meta' in rawResult) return rawResult.data;
  //     return rawResult;  // unwrapped — pass through
  //
  // When `agent_meta` is omitted (which happened on any call where
  // `meta` was all-empty — e.g. fresh session before any observation,
  // or the empty-observation race in `OnCreateTabActionDone`), JS
  // treated the response as already-unwrapped and forwarded `{ data:
  // <observation> }` to the LLM verbatim. Successful calls with
  // populated meta got correctly unwrapped to `{ action, observation,
  // ok }`. The LLM saw two different envelope shapes within one
  // session.
  //
  // Setting an empty dict here keeps the wire shape consistent: every
  // tool response is `{ data: ..., agent_meta: ... }`, and the JS
  // unwrapper always fires. Telemetry consumers that look at specific
  // sub-fields of agent_meta still see the absent fields as missing
  // (the dict is just empty), so this is backward-compatible.
  out.Set("agent_meta", std::move(agent_meta));
  return base::Value(std::move(out));
}

}  // namespace nexus
