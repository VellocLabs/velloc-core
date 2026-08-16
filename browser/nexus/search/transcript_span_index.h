#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace nexus {

// Semantic classification of a searchable byte range inside a
// transcript.jsonl file. The searcher iterates spans of one kind at a
// time (user text first, then assistant text, then tool data), so the
// index stores each kind in its own contiguous vector rather than
// tagging a shared one.
enum class TranscriptSpanKind : uint8_t {
  kUserText,
  kAssistantText,
  // Host notices the transcript records as their own lines — "User has
  // approved exiting plan mode", interrupts, and similar. They read as
  // part of the conversation, so they are searchable.
  kSystemText,
  kToolCall,
  kToolResult,
};

// One searchable byte range. Offsets are absolute file offsets over the
// RAW serialized bytes — string values keep their JSON escapes
// (`hello\nworld`, `\"`), and object values (tool args) span from `{`
// to `}` inclusive. Matching runs over these raw bytes; only emitted
// snippets are unescaped.
struct TranscriptSpan {
  uint64_t begin = 0;
  uint64_t end = 0;  // one past the last byte
  uint32_t line_ordinal = 0;
};

// Per message line: the jump anchor (top-level `uuid`) plus the line's
// byte range. Only `user` / `assistant` / `system` lines get an entry —
// meta and attachment lines carry no searchable content.
struct TranscriptLineMeta {
  uint64_t line_begin = 0;
  uint64_t line_end = 0;
  uint32_t line_ordinal = 0;
  std::string uuid;
};

// The in-memory span index for one transcript.jsonl. Append-only, like
// the file it describes: a tail append re-runs the indexer over
// `[indexed_end, new_size)` only.
struct TranscriptSpanIndex {
  TranscriptSpanIndex();
  TranscriptSpanIndex(TranscriptSpanIndex&&) noexcept;
  TranscriptSpanIndex& operator=(TranscriptSpanIndex&&) noexcept;
  TranscriptSpanIndex(const TranscriptSpanIndex&) = delete;
  TranscriptSpanIndex& operator=(const TranscriptSpanIndex&) = delete;
  ~TranscriptSpanIndex();

  std::vector<TranscriptSpan> user_text;
  std::vector<TranscriptSpan> assistant_text;
  std::vector<TranscriptSpan> system_text;
  std::vector<TranscriptSpan> tool_call;
  std::vector<TranscriptSpan> tool_result;
  std::vector<TranscriptLineMeta> lines;

  // Resume offset: one past the newline of the last COMPLETE line
  // consumed. A trailing unterminated line (an append in flight) is not
  // indexed; the next chunk re-presents it from this offset.
  uint64_t indexed_end = 0;

  // Ordinal of the next physical line. Counts every line — meta and
  // attachment lines included — so an ordinal is a real line number in
  // the file.
  uint32_t next_line_ordinal = 0;

  const std::vector<TranscriptSpan>& SpansFor(TranscriptSpanKind kind) const;

  // Rough retained size, used by the engine's cache LRU.
  size_t ApproxBytes() const;
};

// Scans `data` — the file's bytes from offset `index->indexed_end`
// onward — and appends spans + line metadata for every complete line.
// One sequential pass, no JSON allocation: a structural mini-scanner
// tracks string/escape state, brace depth and the current key path.
//
// Per line it classifies (writer format is the Vercel-style transcript
// of nexus_web/src/utils/sessionStorage.ts):
//   - top-level `type`: `user` / `assistant` / `system` lines are
//     indexed; `__session_meta`, `custom-title`, `ai-title`,
//     `attachment` and unknown types are skipped (they still advance
//     the ordinal).
//   - a `system` line carries its text in a TOP-LEVEL `message` STRING
//     (not `message.content`) — e.g. {"type":"system","subtype":
//     "browser_input","message":"User has approved …"} — so that string
//     is the whole searchable span.
//   - top-level `isMeta: true` rows are skipped entirely — injected
//     context, not something a user searches for.
//   - `message.content` as a plain string → kUserText / kAssistantText
//     by row type.
//   - `message.content[i]` blocks, classified by the block's `type`
//     wherever that key sits in the block (the writer emits
//     `{"text":…,"type":"text"}` — value before type):
//       text                    → its `text` value
//       tool-call / tool_use    → `toolName`/`name` value + full
//                                 `args`/`input` value range
//       tool-result/tool_result → `result`/`content`/`output` value
//       reasoning / thinking    → skipped
// Malformed lines contribute no spans (ordinal still advances).
void IndexTranscriptChunk(std::string_view data, TranscriptSpanIndex* index);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_H_
