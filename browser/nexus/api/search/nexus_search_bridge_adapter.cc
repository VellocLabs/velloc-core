#include "custom_browser/browser/nexus/api/search/nexus_search_bridge_adapter.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_search_service_launcher.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

// `snippet` and `messageUuid` are cut from raw transcript bytes and reach
// us as mojo strings, which are documented as UTF-8 but not validated on
// the wire. base::Value's string constructor DCHECKs on anything that is
// not valid UTF-8 — so one malformed byte in one transcript would take
// the browser down instead of surfacing a bad result. The engine already
// snaps snippet windows to character boundaries; this is the boundary
// guard that makes the crash impossible regardless.
std::string EnsureUtf8(std::string value) {
  if (base::IsStringUTF8AllowingNoncharacters(value)) {
    return value;
  }
  return base::UTF16ToUTF8(base::UTF8ToUTF16(value));
}

const char* KindLabel(mojom::SearchSpanKind kind) {
  switch (kind) {
    case mojom::SearchSpanKind::kAssistantText:
      return "assistantText";
    case mojom::SearchSpanKind::kSystemText:
      return "systemText";
    case mojom::SearchSpanKind::kToolCall:
      return "toolCall";
    case mojom::SearchSpanKind::kToolResult:
      return "toolResult";
    case mojom::SearchSpanKind::kUserText:
      return "userText";
  }
}

// Runs on UI when the service replies; touches only `done` and the
// result, never the adapter, so adapter teardown racing an in-flight
// search is safe (`done` weak-guards its own handler).
void RespondQueryTranscripts(BridgeResponseCallback done,
                             int32_t client_generation,
                             mojom::SearchResultPtr result) {
  if (!result || !result->success) {
    std::string code = result && !result->error_code.empty()
                           ? result->error_code
                           : std::string("SEARCH_ERROR");
    std::string message = result && !result->error.empty()
                              ? result->error
                              : std::string("Search failed");
    std::move(done).Error(code, message);
    return;
  }
  base::ListValue matches;
  for (const mojom::SearchMatchPtr& match : result->matches) {
    Dict row;
    row.Set("sessionId", match->session_id);
    row.Set("projectDir", match->project_dir);
    row.Set("transcriptPath", match->transcript_path);
    row.Set("lineOrdinal", static_cast<int>(match->line_ordinal));
    row.Set("messageUuid", EnsureUtf8(match->message_uuid));
    row.Set("spanKind", KindLabel(match->span_kind));
    // Repair the snippet, then clamp the hit offsets to the repaired
    // length: EnsureUtf8's UTF-16 round-trip can change the byte count, and
    // a matchStart/matchLen past the end would mis-highlight in the guest.
    // Mirrors the clamp the engine's own repair applies.
    std::string snippet = EnsureUtf8(match->snippet);
    const auto snippet_size = static_cast<int32_t>(snippet.size());
    const int32_t match_start =
        std::min(std::max(0, match->match_start), snippet_size);
    const int32_t match_len =
        std::min(std::max(0, match->match_len), snippet_size - match_start);
    row.Set("snippet", std::move(snippet));
    row.Set("matchStart", match_start);
    row.Set("matchLen", match_len);
    matches.Append(std::move(row));
  }
  Dict envelope;
  envelope.Set("generation", client_generation);
  envelope.Set("superseded", result->superseded);
  envelope.Set("truncated", result->truncated);
  envelope.Set("skippedFiles", result->skipped_files);
  envelope.Set("matches", std::move(matches));
  std::move(done).Success(Value(std::move(envelope)));
}

}  // namespace

NexusSearchBridgeAdapter::NexusSearchBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::SearchApiBase(delegate), profile_(profile) {}

NexusSearchBridgeAdapter::~NexusSearchBridgeAdapter() = default;

void NexusSearchBridgeAdapter::HandleQueryTranscriptsOnUI(
    ::nexus::generated::QueryTranscriptsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("SEARCH_ERROR", "Search is unavailable");
    return;
  }

  // The guest's ordering IS the search order — preserved verbatim.
  std::vector<base::FilePath> targets;
  targets.reserve(input.targets.size());
  for (const std::string& target : input.targets) {
    targets.push_back(base::FilePath::FromUTF8Unsafe(target));
  }

  auto reply = base::BindOnce(&RespondQueryTranscripts, std::move(done),
                              input.generation);
  // If the search process dies mid-query mojo drops the reply callback
  // without running it, which would leave the guest's promise pending and
  // the popup spinning forever. Answer with an error instead; the next
  // keystroke relaunches the process.
  auto guarded = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      std::move(reply), mojom::SearchResultPtr());

  GetSharedSearchLauncher(profile_).GetService()->QueryTranscripts(
      std::move(targets), std::move(input.query), input.regex.value_or(false),
      input.caseSensitive.value_or(false), input.maxResults.value_or(0),
      std::move(guarded));
}

}  // namespace nexus
