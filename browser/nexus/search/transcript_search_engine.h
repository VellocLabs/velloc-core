#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SEARCH_ENGINE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SEARCH_ENGINE_H_

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/memory_mapped_file.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/search/transcript_span_index.h"

namespace nexus {

// The compiled query handed to each worker. Defined in the .cc so RE2
// stays out of this header.
struct TranscriptMatchSpec;

struct TranscriptSearchQuery {
  TranscriptSearchQuery();
  TranscriptSearchQuery(TranscriptSearchQuery&&) noexcept;
  TranscriptSearchQuery& operator=(TranscriptSearchQuery&&) noexcept;
  ~TranscriptSearchQuery();

  // Absolute transcript.jsonl paths to search, IN PRIORITY ORDER. The
  // guest owns the ordering (it already caches the project/session list
  // at boot): the session on screen first, then the rest by update time.
  // The engine never enumerates or re-sorts — it walks this list as
  // given, so the caps below cut the tail the guest deemed least
  // relevant. Paths that cannot be opened are counted in `skipped_files`.
  std::vector<base::FilePath> targets;
  std::string query;
  bool regex = false;
  bool case_sensitive = false;
  // Total match cap across all files (per-file cap is fixed).
  int32_t max_total = 500;
  // The engine-side generation this query runs as; a later query bumps
  // the shared counter and this one bails at its next checkpoint.
  uint64_t generation = 0;
};

struct TranscriptSearchMatch {
  TranscriptSearchMatch();
  TranscriptSearchMatch(TranscriptSearchMatch&&) noexcept;
  TranscriptSearchMatch(const TranscriptSearchMatch&);
  TranscriptSearchMatch& operator=(TranscriptSearchMatch&&) noexcept;
  TranscriptSearchMatch& operator=(const TranscriptSearchMatch&);
  ~TranscriptSearchMatch();

  std::string session_id;
  std::string project_dir;
  std::string transcript_path;
  uint32_t line_ordinal = 0;
  std::string message_uuid;
  // "userText" | "assistantText" | "toolCall" | "toolResult"
  std::string span_kind;
  // Context around the hit, JSON-unescaped, single-line.
  std::string snippet;
  int32_t match_start = 0;  // offsets into `snippet` (bytes)
  int32_t match_len = 0;
};

struct TranscriptSearchResult {
  TranscriptSearchResult();
  TranscriptSearchResult(TranscriptSearchResult&&) noexcept;
  TranscriptSearchResult& operator=(TranscriptSearchResult&&) noexcept;
  ~TranscriptSearchResult();

  bool success = false;
  std::string error_code;
  std::string error;
  // A newer query took over; matches may be partial and the client
  // discards the response anyway.
  bool superseded = false;
  bool truncated = false;
  int32_t skipped_files = 0;
  std::vector<TranscriptSearchMatch> matches;
};

// The span-index transcript searcher. Its per-file span cache is confined to
// one sequence, so it needs no lock; RefCounted so an in-flight search outlives
// an adapter teardown. Three passes per file (user text, assistant text, tool
// call/result) fill the caps in that order.
class TranscriptSearchEngine
    : public base::RefCountedThreadSafe<TranscriptSearchEngine> {
 public:
  TranscriptSearchEngine();

  TranscriptSearchEngine(const TranscriptSearchEngine&) = delete;
  TranscriptSearchEngine& operator=(const TranscriptSearchEngine&) = delete;

  // Bumps the shared generation counter and returns the new value.
  // Called on UI before posting; the returned value goes into
  // TranscriptSearchQuery::generation.
  uint64_t BeginQuery();

  // Runs the search. Must be called on the adapter's dedicated
  // sequence; blocking I/O allowed there.
  TranscriptSearchResult Search(const TranscriptSearchQuery& query);

  // Test hooks: number of cached files / how many files were indexed (a
  // tail append counts one, over the tail only).
  size_t CachedFileCountForTesting() const { return cache_.size(); }
  int IndexRunsForTesting() const { return index_runs_; }

 private:
  friend class base::RefCountedThreadSafe<TranscriptSearchEngine>;
  ~TranscriptSearchEngine();

  struct CacheEntry {
    TranscriptSpanIndex index;
    int64_t file_size = 0;
    base::Time mtime;
    uint64_t lru_tick = 0;
  };

  // One transcript's unit of work.
  //
  // Ownership is what keeps this lock-free: the owning sequence allocates
  // every job, MOVES that file's cached index into it, and moves the
  // index back out when the worker is done. While a job is in flight it
  // is the sole owner of its index, and no worker can see the cache, the
  // result list, or another job — so there is no shared array to guard
  // and no lock anywhere on the search path.
  struct TargetJob {
    TargetJob();
    TargetJob(TargetJob&&) noexcept;
    TargetJob& operator=(TargetJob&&) noexcept;
    ~TargetJob();

    // Handed in by the owner.
    base::FilePath path;
    TranscriptSpanIndex index;
    int64_t file_size = 0;
    base::Time mtime;

    // Filled in by the worker.
    std::vector<TranscriptSearchMatch> matches;
    bool skipped = false;
    bool reindexed = false;
  };

  // Maps the file, brings `job->index` up to date (full build, or a
  // tail-only rescan after an append) and collects its matches. Static:
  // it can reach nothing but the job it was handed.
  static void RunTargetJob(TargetJob* job, const TranscriptMatchSpec& spec);

  // Runs jobs[begin, end) — one worker's share. `latest_generation` is
  // read-only, the single piece of shared state a worker can observe, and
  // only to notice it has been superseded.
  static void RunTargetJobs(base::span<TargetJob> jobs,
                            size_t begin,
                            size_t end,
                            const TranscriptMatchSpec& spec,
                            const std::atomic<uint64_t>* latest_generation,
                            uint64_t generation);

  void EvictOverBudget();

  // Sequence-confined: only ever touched on the adapter's task runner,
  // between dispatches, never while a worker is in flight.
  std::map<base::FilePath, CacheEntry> cache_;
  uint64_t lru_tick_ = 0;
  int index_runs_ = 0;

  std::atomic<uint64_t> latest_generation_{0};
};

// Exposed for tests: decodes the JSON string escapes in `raw` (a
// fragment of a serialized JSON string value) into a single-line
// snippet, remapping the byte range [match_begin, match_end) into
// decoded coordinates.
std::string UnescapeSnippetFragment(std::string_view raw,
                                    size_t match_begin,
                                    size_t match_end,
                                    int32_t* out_match_start,
                                    int32_t* out_match_len);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SEARCH_ENGINE_H_
