#include "custom_browser/browser/nexus/search/transcript_search_engine.h"

#include "custom_browser/browser/nexus/search/transcript_span_index_file.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/barrier_closure.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/system/sys_info.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "third_party/re2/src/re2/re2.h"

namespace nexus {

namespace {

// CC GlobalSearchDialog parity.
constexpr int kMaxMatchesPerFile = 10;
constexpr int32_t kDefaultMaxTotal = 500;
constexpr int32_t kMaxTotalCeiling = 2000;

// Sanity bound on the guest-supplied target list. The guest orders by
// relevance, so an over-long list is cut from the BACK — the sessions it
// ranked least relevant — and the response is flagged `truncated`.
constexpr size_t kMaxTargets = 2000;

// Snippet context on each side of the hit, in raw bytes.
constexpr size_t kSnippetContext = 120;

// Cache LRU budget (retained span/line vectors, not file bytes).
constexpr size_t kCacheBudgetBytes = 32u * 1024 * 1024;

// Below this many transcripts the fan-out costs more than it saves.
constexpr size_t kMinTargetsForParallel = 4;

// Hard ceiling on workers per search. Search is a background convenience
// on a machine that is also rendering a browser, so it takes a slice of
// the CPU and never the whole thing: cores-2, capped here. The rest of
// the load shedding is the thread pool's job — shares post at
// USER_VISIBLE, so the scheduler already yields to higher-priority
// browser work. (Polling the OS for CPU load per query would be costly,
// platform-specific, and stale by the time it was read.)
constexpr int kMaxSearchWorkers = 4;

size_t SearchConcurrency(size_t target_count) {
  if (target_count < kMinTargetsForParallel) {
    return 1;
  }
  const int cores = base::SysInfo::NumberOfProcessors();
  const int budget = std::clamp(cores - 2, 1, kMaxSearchWorkers);
  return std::min(static_cast<size_t>(budget), target_count);
}

const char* SpanKindLabel(TranscriptSpanKind kind) {
  switch (kind) {
    case TranscriptSpanKind::kUserText:
      return "userText";
    case TranscriptSpanKind::kAssistantText:
      return "assistantText";
    case TranscriptSpanKind::kSystemText:
      return "systemText";
    case TranscriptSpanKind::kToolCall:
      return "toolCall";
    case TranscriptSpanKind::kToolResult:
      return "toolResult";
  }
  return "userText";
}

// Case-insensitive (ASCII) substring find over raw bytes. Returns
// std::string_view::npos when absent. `needle` must already be
// lower-cased.
size_t FindFoldedAscii(std::string_view haystack,
                       std::string_view lowered_needle,
                       size_t from) {
  if (lowered_needle.empty() || haystack.size() < lowered_needle.size()) {
    return std::string_view::npos;
  }
  const size_t last_start = haystack.size() - lowered_needle.size();
  for (size_t i = from; i <= last_start; ++i) {
    size_t j = 0;
    while (j < lowered_needle.size() &&
           base::ToLowerASCII(haystack[i + j]) == lowered_needle[j]) {
      ++j;
    }
    if (j == lowered_needle.size()) {
      return i;
    }
  }
  return std::string_view::npos;
}

// True if `pos` in `data` sits immediately after an ODD run of
// backslashes — i.e. inside an escape sequence.
bool InsideEscape(std::string_view data, size_t begin, size_t pos) {
  size_t backslashes = 0;
  while (pos > begin && data[pos - 1 - backslashes] == '\\') {
    ++backslashes;
    if (pos - 1 - backslashes + 1 == begin) {
      break;
    }
  }
  return (backslashes % 2) == 1;
}

// How far the window may slide to reach a clean break. Beyond this the
// text has no spaces near the edge (CJK, a long identifier), and moving
// further would cost more context than the ragged edge does.
constexpr size_t kBoundaryBudget = 48;

// Slides the window's START forward to a readable beginning: just after a
// sentence break if one is close, else the start of the next whole word.
// A snippet that opens mid-word reads as garbage, and the window is a
// blunt byte count, so it lands mid-word most of the time.
size_t SnapStartToBoundary(std::string_view piece, size_t begin, size_t limit) {
  if (begin == 0) {
    return begin;
  }
  const size_t ceiling = std::min(limit, begin + kBoundaryBudget);
  for (size_t i = begin; i + 1 < ceiling; ++i) {
    const char c = piece[i];
    const bool sentence_end =
        c == '.' || c == '!' || c == '?' || c == ';' || c == ',';
    if (sentence_end && piece[i + 1] == ' ') {
      return i + 2;
    }
  }
  for (size_t i = begin; i < ceiling; ++i) {
    if (piece[i] == ' ') {
      return i + 1;
    }
  }
  return begin;
}

// Pulls the window's END back to the end of the last whole word.
size_t SnapEndToBoundary(std::string_view piece, size_t end, size_t floor) {
  if (end >= piece.size()) {
    return end;
  }
  const size_t bottom = end > kBoundaryBudget ? end - kBoundaryBudget : 0;
  for (size_t i = end; i > std::max(floor, bottom); --i) {
    if (piece[i] == ' ') {
      return i;
    }
  }
  return end;
}

// A UTF-8 continuation byte (10xxxxxx) — i.e. NOT a character boundary.
bool IsUtf8Continuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Binary-searches the line metadata for the line containing `offset`.
const TranscriptLineMeta* FindLine(const std::vector<TranscriptLineMeta>& lines,
                                   uint64_t offset) {
  auto it = std::upper_bound(
      lines.begin(), lines.end(), offset,
      [](uint64_t value, const TranscriptLineMeta& line) {
        return value < line.line_begin;
      });
  if (it == lines.begin()) {
    return nullptr;
  }
  --it;
  return offset < it->line_end ? &*it : nullptr;
}

}  // namespace

// The compiled query, shared read-only by every worker.
struct TranscriptMatchSpec {
  raw_ptr<const re2::RE2> regex = nullptr;
  std::string_view literal;
  std::string_view lowered;
  bool case_sensitive = false;
};

TranscriptSearchEngine::TargetJob::TargetJob() = default;
TranscriptSearchEngine::TargetJob::TargetJob(TargetJob&&) noexcept = default;
TranscriptSearchEngine::TargetJob&
TranscriptSearchEngine::TargetJob::operator=(TargetJob&&) noexcept = default;
TranscriptSearchEngine::TargetJob::~TargetJob() = default;

TranscriptSearchQuery::TranscriptSearchQuery() = default;
TranscriptSearchQuery::TranscriptSearchQuery(TranscriptSearchQuery&&) noexcept =
    default;
TranscriptSearchQuery& TranscriptSearchQuery::operator=(
    TranscriptSearchQuery&&) noexcept = default;
TranscriptSearchQuery::~TranscriptSearchQuery() = default;

TranscriptSearchMatch::TranscriptSearchMatch() = default;
TranscriptSearchMatch::TranscriptSearchMatch(TranscriptSearchMatch&&) noexcept =
    default;
TranscriptSearchMatch::TranscriptSearchMatch(const TranscriptSearchMatch&) =
    default;
TranscriptSearchMatch& TranscriptSearchMatch::operator=(
    TranscriptSearchMatch&&) noexcept = default;
TranscriptSearchMatch& TranscriptSearchMatch::operator=(
    const TranscriptSearchMatch&) = default;
TranscriptSearchMatch::~TranscriptSearchMatch() = default;

TranscriptSearchResult::TranscriptSearchResult() = default;
TranscriptSearchResult::TranscriptSearchResult(
    TranscriptSearchResult&&) noexcept = default;
TranscriptSearchResult& TranscriptSearchResult::operator=(
    TranscriptSearchResult&&) noexcept = default;
TranscriptSearchResult::~TranscriptSearchResult() = default;

std::string UnescapeSnippetFragment(std::string_view raw,
                                    size_t match_begin,
                                    size_t match_end,
                                    int32_t* out_match_start,
                                    int32_t* out_match_len) {
  std::string decoded;
  decoded.reserve(raw.size());
  int32_t decoded_match_start = 0;
  int32_t decoded_match_end = 0;

  size_t i = 0;
  while (i < raw.size()) {
    if (i == match_begin) {
      decoded_match_start = static_cast<int32_t>(decoded.size());
    }
    if (i == match_end) {
      decoded_match_end = static_cast<int32_t>(decoded.size());
    }
    const char c = raw[i];
    if (c != '\\' || i + 1 >= raw.size()) {
      // Newlines never appear raw inside a JSON string, but the snippet
      // window can only start/end inside one string value, so plain
      // bytes pass through.
      decoded.push_back(c);
      ++i;
      continue;
    }
    const char esc = raw[i + 1];
    switch (esc) {
      case 'n':
      case 'r':
      case 't':
      case 'b':
      case 'f':
        decoded.push_back(' ');  // single-line snippet
        i += 2;
        break;
      case '"':
      case '\\':
      case '/':
        decoded.push_back(esc);
        i += 2;
        break;
      case 'u': {
        if (i + 6 > raw.size()) {
          i = raw.size();  // truncated escape at fragment edge
          break;
        }
        int code = 0;
        bool ok = true;
        for (size_t k = 0; k < 4; ++k) {
          const int v = HexValue(raw[i + 2 + k]);
          if (v < 0) {
            ok = false;
            break;
          }
          code = code * 16 + v;
        }
        if (!ok) {
          decoded.push_back('?');
          i += 2;
          break;
        }
        size_t consumed = 6;
        base_icu::UChar32 code_point = code;
        // Surrogate pair: join with the following \uXXXX low surrogate.
        if (code >= 0xD800 && code <= 0xDBFF && i + 12 <= raw.size() &&
            raw[i + 6] == '\\' && raw[i + 7] == 'u') {
          int low = 0;
          bool low_ok = true;
          for (size_t k = 0; k < 4; ++k) {
            const int v = HexValue(raw[i + 8 + k]);
            if (v < 0) {
              low_ok = false;
              break;
            }
            low = low * 16 + v;
          }
          if (low_ok && low >= 0xDC00 && low <= 0xDFFF) {
            code_point =
                0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            consumed = 12;
          }
        }
        if (code_point >= 0xD800 && code_point <= 0xDFFF) {
          decoded.push_back('?');  // unpaired surrogate
        } else {
          base::WriteUnicodeCharacter(code_point, &decoded);
        }
        i += consumed;
        break;
      }
      default:
        decoded.push_back(esc);
        i += 2;
        break;
    }
  }
  if (match_begin >= raw.size()) {
    decoded_match_start = static_cast<int32_t>(decoded.size());
  }
  if (match_end >= raw.size()) {
    decoded_match_end = static_cast<int32_t>(decoded.size());
  }
  *out_match_start = decoded_match_start;
  *out_match_len = std::max(0, decoded_match_end - decoded_match_start);
  // Last line of defence. The caller snaps the window to character
  // boundaries, but a transcript can also hold genuinely malformed bytes,
  // and handing base::Value a non-UTF-8 string is a DCHECK, not an error
  // return — so repair rather than crash. The round-trip substitutes
  // U+FFFD for anything invalid; the offsets above may shift by a byte or
  // two in that case, which the renderer clamps.
  if (!base::IsStringUTF8AllowingNoncharacters(decoded)) {
    decoded = base::UTF16ToUTF8(base::UTF8ToUTF16(decoded));
    *out_match_start =
        std::min(*out_match_start, static_cast<int32_t>(decoded.size()));
    *out_match_len =
        std::min(*out_match_len,
                 static_cast<int32_t>(decoded.size()) - *out_match_start);
  }
  return decoded;
}

TranscriptSearchEngine::TranscriptSearchEngine() = default;
TranscriptSearchEngine::~TranscriptSearchEngine() = default;

uint64_t TranscriptSearchEngine::BeginQuery() {
  return latest_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
}

void TranscriptSearchEngine::RunTargetJob(TargetJob* job,
                                         const TranscriptMatchSpec& spec) {
  // Share write+delete so the live session writer never collides with a
  // search read. (base::File on Windows shares read/write by default;
  // FLAG_WIN_SHARE_DELETE adds delete.)
  base::File file(job->path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                                 base::File::FLAG_WIN_SHARE_DELETE);
  if (!file.IsValid()) {
    job->skipped = true;
    return;
  }
  base::File::Info info;
  if (!file.GetInfo(&info) || info.size <= 0) {
    job->skipped = true;
    return;
  }
  const int64_t size = info.size;

  // Zero-copy: map the file and both index and match in place over the
  // mapping — no byte copies. Clamping the view to the stat'd size keeps
  // a concurrent append's torn tail invisible.
  base::MemoryMappedFile mapping;
  if (!mapping.Initialize(std::move(file))) {
    job->skipped = true;
    return;
  }
  const size_t view_size =
      std::min(mapping.length(), static_cast<size_t>(size));
  const std::string_view data(reinterpret_cast<const char*>(mapping.data()),
                              view_size);

  // Nothing in memory for this transcript (first search since launch):
  // try the `.sdi` sidecar before scanning. The stamps it carries go
  // through exactly the same validation as the in-memory ones below, so
  // a stale sidecar is tail-extended or rebuilt rather than trusted.
  if (job->index.indexed_end == 0 && job->file_size == 0) {
    int64_t sidecar_size = 0;
    base::Time sidecar_mtime;
    if (ReadSpanIndexFile(SpanIndexFilePathFor(job->path), &job->index,
                          &sidecar_size, &sidecar_mtime)) {
      job->file_size = sidecar_size;
      job->mtime = sidecar_mtime;
    }
  }

  const bool shrunk_or_replaced =
      size < job->file_size ||
      (size == job->file_size && info.last_modified != job->mtime) ||
      static_cast<uint64_t>(size) < job->index.indexed_end;
  if (shrunk_or_replaced) {
    job->index = TranscriptSpanIndex();
  }
  if (job->index.indexed_end < data.size()) {
    // Fresh file or tail append: index only [indexed_end, size).
    IndexTranscriptChunk(data.substr(job->index.indexed_end), &job->index);
    job->reindexed = true;
  }
  job->file_size = size;
  job->mtime = info.last_modified;

  // Persist only what changed. A search that found the sidecar current
  // writes nothing, so the common case costs one read and no write.
  if (job->reindexed) {
    WriteSpanIndexFile(SpanIndexFilePathFor(job->path), job->index, size,
                       info.last_modified);
  }

  static constexpr TranscriptSpanKind kPassOrder[] = {
      TranscriptSpanKind::kUserText,
      TranscriptSpanKind::kAssistantText,
      TranscriptSpanKind::kSystemText,
      TranscriptSpanKind::kToolCall,
      TranscriptSpanKind::kToolResult,
  };

  const std::string session_id =
      job->path.DirName().BaseName().AsUTF8Unsafe();
  const std::string project_dir =
      job->path.DirName().DirName().BaseName().AsUTF8Unsafe();
  const std::string transcript_path = job->path.AsUTF8Unsafe();

  int file_matches = 0;
  for (TranscriptSpanKind kind : kPassOrder) {
    if (file_matches >= kMaxMatchesPerFile) {
      break;
    }
    for (const TranscriptSpan& span : job->index.SpansFor(kind)) {
      if (file_matches >= kMaxMatchesPerFile) {
        break;
      }
      // The index can momentarily be ahead of this read (another writer
      // appended between stat and read); clamp defensively.
      if (span.end > data.size()) {
        continue;
      }
      const std::string_view piece =
          data.substr(span.begin, span.end - span.begin);
      size_t from = 0;
      while (from < piece.size()) {
        size_t hit_begin = std::string_view::npos;
        size_t hit_end = std::string_view::npos;
        if (spec.regex) {
          std::string_view submatch;
          if (spec.regex->Match(piece, from, piece.size(),
                                re2::RE2::UNANCHORED, &submatch, 1)) {
            hit_begin = static_cast<size_t>(submatch.data() - piece.data());
            hit_end = hit_begin + submatch.size();
          }
        } else if (spec.case_sensitive) {
          const size_t found = piece.find(spec.literal, from);
          if (found != std::string_view::npos) {
            hit_begin = found;
            hit_end = found + spec.literal.size();
          }
        } else {
          const size_t found = FindFoldedAscii(piece, spec.lowered, from);
          if (found != std::string_view::npos) {
            hit_begin = found;
            hit_end = found + spec.lowered.size();
          }
        }
        if (hit_begin == std::string_view::npos) {
          break;
        }

        // Snippet window, clamped to the span, nudged off any escape
        // sequence it would otherwise split.
        size_t ctx_begin =
            hit_begin > kSnippetContext ? hit_begin - kSnippetContext : 0;
        size_t ctx_end = std::min(piece.size(), hit_end + kSnippetContext);
        while (ctx_begin > 0 && InsideEscape(piece, 0, ctx_begin)) {
          --ctx_begin;
        }
        // Then snap both edges to UTF-8 character boundaries. The window
        // is a fixed byte count, so on any multi-byte text it routinely
        // lands mid-character — and a snippet carrying half a character
        // is invalid UTF-8, which base::Value refuses with a DCHECK (a
        // debug-build crash on the whole search). Walk the leading edge
        // forward off continuation bytes and the trailing edge back.
        while (ctx_begin < hit_begin && IsUtf8Continuation(piece[ctx_begin])) {
          ++ctx_begin;
        }
        while (ctx_end > hit_end && ctx_end < piece.size() &&
               IsUtf8Continuation(piece[ctx_end])) {
          --ctx_end;
        }
        // Finally, whole words. Both snaps land on ASCII spaces, so they
        // cannot undo the character alignment above.
        ctx_begin = SnapStartToBoundary(piece, ctx_begin, hit_begin);
        ctx_end = SnapEndToBoundary(piece, ctx_end, hit_end);

        TranscriptSearchMatch match;
        match.session_id = session_id;
        match.project_dir = project_dir;
        match.transcript_path = transcript_path;
        match.span_kind = SpanKindLabel(kind);
        match.snippet = UnescapeSnippetFragment(
            piece.substr(ctx_begin, ctx_end - ctx_begin), hit_begin - ctx_begin,
            hit_end - ctx_begin, &match.match_start, &match.match_len);
        match.line_ordinal = span.line_ordinal;
        if (const TranscriptLineMeta* line =
                FindLine(job->index.lines, span.begin)) {
          match.message_uuid = line->uuid;
        }
        job->matches.push_back(std::move(match));
        ++file_matches;

        // Guarantee progress even on empty regex matches.
        from = std::max(hit_end, hit_begin + 1);
        if (file_matches >= kMaxMatchesPerFile) {
          break;
        }
      }
    }
  }
}

void TranscriptSearchEngine::RunTargetJobs(
    base::span<TargetJob> jobs,
    size_t begin,
    size_t end,
    const TranscriptMatchSpec& spec,
    const std::atomic<uint64_t>* latest_generation,
    uint64_t generation) {
  for (size_t i = begin; i < end; ++i) {
    // Cheap cancellation checkpoint: a newer keystroke has taken over, so
    // this worker's remaining files are wasted work. The jobs it leaves
    // untouched keep their indexes and go back into the cache intact.
    if (latest_generation->load(std::memory_order_relaxed) != generation) {
      return;
    }
    RunTargetJob(&jobs[i], spec);
  }
}

void TranscriptSearchEngine::EvictOverBudget() {
  size_t total = 0;
  for (const auto& [path, entry] : cache_) {
    total += entry.index.ApproxBytes();
  }
  while (total > kCacheBudgetBytes && cache_.size() > 1) {
    auto oldest = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->second.lru_tick < oldest->second.lru_tick) {
        oldest = it;
      }
    }
    total -= oldest->second.index.ApproxBytes();
    cache_.erase(oldest);
  }
}

TranscriptSearchResult TranscriptSearchEngine::Search(
    const TranscriptSearchQuery& query) {
  TranscriptSearchResult result;
  if (query.query.empty()) {
    result.success = true;
    return result;
  }

  std::unique_ptr<re2::RE2> regex;
  if (query.regex) {
    re2::RE2::Options options;
    options.set_case_sensitive(query.case_sensitive);
    options.set_log_errors(false);
    options.set_max_mem(8 << 20);
    regex = std::make_unique<re2::RE2>(query.query, options);
    if (!regex->ok()) {
      result.error_code = "SEARCH_BAD_PATTERN";
      result.error = regex->error();
      return result;
    }
  }
  const std::string lowered_needle =
      query.case_sensitive ? query.query : base::ToLowerASCII(query.query);

  const int32_t max_total = query.max_total > 0
                                ? std::min(query.max_total, kMaxTotalCeiling)
                                : kDefaultMaxTotal;

  // The guest supplies the target list already ordered by relevance, so
  // this is a straight walk — no enumeration, no stat, no re-sort. Over
  // the cap we search the FRONT of the list (the guest's most relevant)
  // and report the tail as truncated.
  base::span<const base::FilePath> targets(query.targets);
  if (targets.size() > kMaxTargets) {
    targets = targets.first(kMaxTargets);
    result.truncated = true;
  }
  if (targets.empty()) {
    result.success = true;
    return result;
  }

  // RE2 matching is thread-safe, so one compiled pattern serves every
  // worker; the needles are views into locals that outlive the join.
  const TranscriptMatchSpec spec{regex.get(), query.query, lowered_needle,
                                 query.case_sensitive};

  // Allocate every job here, on the owning sequence, moving each file's
  // cached index INTO its job. From here until the join the cache is
  // empty of those entries and no worker can reach it, so nothing is
  // shared and nothing needs locking.
  std::vector<TargetJob> jobs(targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    jobs[i].path = targets[i];
    auto it = cache_.find(targets[i]);
    if (it != cache_.end()) {
      jobs[i].index = std::move(it->second.index);
      jobs[i].file_size = it->second.file_size;
      jobs[i].mtime = it->second.mtime;
      cache_.erase(it);
    }
  }

  const size_t workers = SearchConcurrency(jobs.size());
  if (workers <= 1) {
    RunTargetJobs(jobs, 0, jobs.size(), spec, &latest_generation_,
                  query.generation);
  } else {
    // Contiguous shares, one per worker. The last share runs INLINE: the
    // owning sequence would otherwise just block, and it saves a hop.
    base::WaitableEvent joined;
    base::RepeatingClosure barrier = base::BarrierClosure(
        workers,
        base::BindOnce(&base::WaitableEvent::Signal, base::Unretained(&joined)));
    const scoped_refptr<base::TaskRunner> runner =
        base::ThreadPool::CreateTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE});
    const size_t share = (jobs.size() + workers - 1) / workers;
    for (size_t worker = 0; worker < workers; ++worker) {
      const size_t begin = std::min(worker * share, jobs.size());
      if (worker + 1 == workers) {
        RunTargetJobs(jobs, begin, jobs.size(), spec, &latest_generation_,
                      query.generation);
        barrier.Run();
        break;
      }
      const size_t end = std::min(begin + share, jobs.size());
      const bool posted = runner->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::span<TargetJob> jobs, size_t begin, size_t end,
                 const TranscriptMatchSpec* spec,
                 const std::atomic<uint64_t>* latest_generation,
                 uint64_t generation, base::RepeatingClosure barrier) {
                RunTargetJobs(jobs, begin, end, *spec, latest_generation,
                              generation);
                barrier.Run();
              },
              base::span<TargetJob>(jobs), begin, end, &spec,
              &latest_generation_, query.generation, barrier));
      if (!posted) {
        // The pool refused the task — it is shutting down, and the
        // callback (with its barrier) was destroyed unrun. Take the share
        // inline so the count still reaches `workers`; otherwise the
        // Wait() below would block this sequence until process exit.
        RunTargetJobs(jobs, begin, end, spec, &latest_generation_,
                      query.generation);
        barrier.Run();
      }
    }
    // Safe to block: this runs on the adapter's MayBlock +
    // WithBaseSyncPrimitives sequence, and every share signals the
    // barrier even when it cancels early. Blocking is also what makes the
    // job array's lifetime trivially correct.
    joined.Wait();
  }

  // Back on the owning sequence and nothing else is running: move every
  // index home, then merge the matches in TARGET order, which makes the
  // response identical no matter which worker finished first.
  for (TargetJob& job : jobs) {
    if (job.skipped) {
      ++result.skipped_files;
      continue;
    }
    CacheEntry& entry = cache_[job.path];
    entry.index = std::move(job.index);
    entry.file_size = job.file_size;
    entry.mtime = job.mtime;
    entry.lru_tick = ++lru_tick_;
    if (job.reindexed) {
      ++index_runs_;
    }
  }

  if (latest_generation_.load(std::memory_order_relaxed) != query.generation) {
    result.superseded = true;
    result.success = true;
    return result;
  }

  for (TargetJob& job : jobs) {
    for (TranscriptSearchMatch& match : job.matches) {
      if (static_cast<int32_t>(result.matches.size()) >= max_total) {
        result.truncated = true;
        break;
      }
      result.matches.push_back(std::move(match));
    }
  }

  EvictOverBudget();
  result.success = true;
  return result;
}

}  // namespace nexus
