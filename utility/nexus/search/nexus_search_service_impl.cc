// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/search/nexus_search_service_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"

namespace nexus {

namespace {

mojom::SearchSpanKind ToMojoKind(const std::string& label) {
  if (label == "assistantText") {
    return mojom::SearchSpanKind::kAssistantText;
  }
  if (label == "systemText") {
    return mojom::SearchSpanKind::kSystemText;
  }
  if (label == "toolCall") {
    return mojom::SearchSpanKind::kToolCall;
  }
  if (label == "toolResult") {
    return mojom::SearchSpanKind::kToolResult;
  }
  return mojom::SearchSpanKind::kUserText;
}

// Free function, so the reply survives the service being torn down
// underneath an in-flight search — it touches only the callback and the
// result it was handed.
void ReplyWithResult(mojom::NexusSearchService::QueryTranscriptsCallback
                         callback,
                     TranscriptSearchResult found) {
  auto result = mojom::SearchResult::New();
  result->success = found.success;
  result->error_code = found.error_code;
  result->error = found.error;
  result->superseded = found.superseded;
  result->truncated = found.truncated;
  result->skipped_files = found.skipped_files;
  result->matches.reserve(found.matches.size());
  for (TranscriptSearchMatch& match : found.matches) {
    auto entry = mojom::SearchMatch::New();
    entry->session_id = std::move(match.session_id);
    entry->project_dir = std::move(match.project_dir);
    entry->transcript_path = std::move(match.transcript_path);
    entry->line_ordinal = match.line_ordinal;
    entry->message_uuid = std::move(match.message_uuid);
    entry->span_kind = ToMojoKind(match.span_kind);
    entry->snippet = std::move(match.snippet);
    entry->match_start = match.match_start;
    entry->match_len = match.match_len;
    result->matches.push_back(std::move(entry));
  }
  std::move(callback).Run(std::move(result));
}

}  // namespace

NexusSearchServiceImpl::NexusSearchServiceImpl(
    mojo::PendingReceiver<mojom::NexusSearchService> receiver)
    : receiver_(this, std::move(receiver)),
      engine_(base::MakeRefCounted<TranscriptSearchEngine>()),
      search_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::WithBaseSyncPrimitives(),
           base::TaskPriority::USER_VISIBLE})) {}

NexusSearchServiceImpl::~NexusSearchServiceImpl() = default;

void NexusSearchServiceImpl::QueryTranscripts(
    const std::vector<base::FilePath>& targets,
    const std::string& query,
    bool regex,
    bool case_sensitive,
    int32_t max_results,
    QueryTranscriptsCallback callback) {
  TranscriptSearchQuery request;
  request.targets = targets;
  request.query = query;
  request.regex = regex;
  request.case_sensitive = case_sensitive;
  request.max_total = max_results;
  // Bumping the generation here — on the thread that receives calls, not
  // on the search sequence — is what lets a new keystroke cancel a search
  // already running. The superseded search bails at its next per-file
  // checkpoint and replies `{superseded:true}`.
  request.generation = engine_->BeginQuery();

  search_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&TranscriptSearchEngine::Search, engine_,
                     std::move(request)),
      base::BindOnce(&ReplyWithResult, std::move(callback)));
}

}  // namespace nexus
