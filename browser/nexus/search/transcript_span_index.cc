#include "custom_browser/browser/nexus/search/transcript_span_index.h"

#include <optional>
#include <utility>

namespace nexus {

namespace {

// Byte ranges are line-relative while scanning and rebased to absolute
// file offsets at commit time.
struct Range {
  size_t begin = 0;
  size_t end = 0;
  bool valid() const { return end > begin; }
};

// Captured content of one `message.content[i]` block. The block's
// `type` key can appear AFTER the values it classifies (the writer
// emits `{"text":…,"type":"text"}`), so values are buffered and the
// block is committed when its object closes.
struct BlockCapture {
  std::string type;
  Range text;
  Range name;
  Range args;
  Range result;
};

// Everything harvested from one line, committed only if the line turns
// out to be a non-meta user/assistant message.
struct LineCapture {
  std::string line_type;
  std::string uuid;
  bool is_meta = false;
  Range content_string;         // message.content as a plain string
  Range top_level_message;      // `message` as a plain string (system)
  std::vector<Range> texts;     // text blocks
  std::vector<Range> tool_calls;
  std::vector<Range> tool_results;
  bool malformed = false;
};

// One frame per open `{` or `[`. `parent_key` is the object key (or ""
// for array elements) under which this container was opened.
struct Frame {
  bool is_object = false;
  std::string parent_key;
};

bool IsJsonWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Scans a JSON string starting at the opening quote `pos`. Returns the
// index one past the closing quote, or nullopt if the line ends first.
// The contents (between the quotes) are [pos + 1, ret - 1).
std::optional<size_t> ScanString(std::string_view line, size_t pos) {
  ++pos;  // opening quote
  while (pos < line.size()) {
    const char c = line[pos];
    if (c == '\\') {
      pos += 2;  // escape consumes the next byte; \uXXXX has no quotes
      continue;
    }
    if (c == '"') {
      return pos + 1;
    }
    ++pos;
  }
  return std::nullopt;
}

// The structural mini-scanner: walks one JSONL line without building a
// JSON tree, capturing the handful of value ranges the index needs.
class LineScanner {
 public:
  explicit LineScanner(std::string_view line) : line_(line) {}

  // Returns the capture; `malformed` is set on any structural surprise
  // and the caller drops the spans (never crashes, never mis-spans).
  LineCapture Scan() {
    size_t pos = SkipWs(0);
    if (pos >= line_.size() || line_[pos] != '{') {
      out_.malformed = true;
      return std::move(out_);
    }
    if (!ScanValue(pos, /*key=*/"", &pos)) {
      out_.malformed = true;
    }
    return std::move(out_);
  }

 private:
  size_t SkipWs(size_t pos) const {
    while (pos < line_.size() && IsJsonWhitespace(line_[pos])) {
      ++pos;
    }
    return pos;
  }

  // Depth of the container stack. The top-level object is depth 1.
  size_t Depth() const { return stack_.size(); }

  bool InMessageObject() const {
    return Depth() == 2 && stack_[1].is_object &&
           stack_[1].parent_key == "message";
  }

  bool InContentArray() const {
    return Depth() == 3 && !stack_[2].is_object &&
           stack_[2].parent_key == "content" && stack_[1].is_object &&
           stack_[1].parent_key == "message";
  }

  bool InBlockObject() const {
    return Depth() == 4 && stack_[3].is_object && block_.has_value();
  }

  // Parses the value beginning at `pos` (first non-ws already reached),
  // owned by `key` in the enclosing object ("" inside arrays). Writes
  // one past the value into `*end`. Returns false on malformed input.
  bool ScanValue(size_t pos, const std::string& key, size_t* end) {
    const char c = line_[pos];
    if (c == '"') {
      std::optional<size_t> close = ScanString(line_, pos);
      if (!close.has_value()) {
        return false;
      }
      CaptureString(key, Range{pos + 1, *close - 1});
      *end = *close;
      return true;
    }
    if (c == '{' || c == '[') {
      return ScanContainer(pos, key, end);
    }
    // Scalar: number / true / false / null. Scan to the next structural
    // delimiter; content is irrelevant except for isMeta.
    size_t p = pos;
    while (p < line_.size() && line_[p] != ',' && line_[p] != '}' &&
           line_[p] != ']' && !IsJsonWhitespace(line_[p])) {
      ++p;
    }
    if (Depth() == 1 && key == "isMeta") {
      out_.is_meta = line_.substr(pos, p - pos) == "true";
    }
    *end = p;
    return p > pos;
  }

  bool ScanContainer(size_t pos, const std::string& key, size_t* end) {
    const bool is_object = line_[pos] == '{';
    const size_t open = pos;

    // A `message.content[i]` value that is an object is a content
    // block; start buffering it. Blocks do not nest.
    const bool starts_block = is_object && InContentArray();
    if (starts_block) {
      block_.emplace();
    }
    stack_.push_back(Frame{is_object, key});

    pos = SkipWs(pos + 1);
    bool first = true;
    while (true) {
      if (pos >= line_.size()) {
        return false;
      }
      const char close_char = is_object ? '}' : ']';
      if (line_[pos] == close_char) {
        stack_.pop_back();
        *end = pos + 1;
        if (starts_block) {
          CommitBlock();
        }
        CaptureContainer(key, Range{open, pos + 1});
        return true;
      }
      if (!first) {
        if (line_[pos] != ',') {
          return false;
        }
        pos = SkipWs(pos + 1);
      }
      first = false;

      std::string element_key;
      if (is_object) {
        if (pos >= line_.size() || line_[pos] != '"') {
          return false;
        }
        std::optional<size_t> key_close = ScanString(line_, pos);
        if (!key_close.has_value()) {
          return false;
        }
        element_key.assign(line_.substr(pos + 1, *key_close - 1 - (pos + 1)));
        pos = SkipWs(*key_close);
        if (pos >= line_.size() || line_[pos] != ':') {
          return false;
        }
        pos = SkipWs(pos + 1);
      }
      size_t value_end = 0;
      if (pos >= line_.size() || !ScanValue(pos, element_key, &value_end)) {
        return false;
      }
      pos = SkipWs(value_end);
    }
  }

  // String value captured under `key` at the current stack depth.
  void CaptureString(const std::string& key, Range contents) {
    if (Depth() == 1) {
      if (key == "type") {
        out_.line_type.assign(RangeView(contents));
      } else if (key == "uuid") {
        out_.uuid.assign(RangeView(contents));
      } else if (key == "message") {
        // Only a `system` line has a STRING here; on user/assistant rows
        // `message` is an object and this never fires.
        out_.top_level_message = contents;
      }
      return;
    }
    if (InMessageObject() && key == "content") {
      out_.content_string = contents;
      return;
    }
    if (InBlockObject()) {
      BlockCapture& block = *block_;
      if (key == "type") {
        block.type.assign(RangeView(contents));
      } else if (key == "text") {
        block.text = contents;
      } else if (key == "toolName" || key == "name") {
        block.name = contents;
      } else if (key == "args" || key == "input") {
        block.args = contents;
      } else if (key == "result" || key == "content" || key == "output") {
        block.result = contents;
      }
    }
  }

  // Object/array value captured under `key`. Only tool args / results
  // need whole-container ranges.
  void CaptureContainer(const std::string& key, Range whole) {
    if (!InBlockObject()) {
      return;
    }
    BlockCapture& block = *block_;
    if (key == "args" || key == "input") {
      block.args = whole;
    } else if (key == "result" || key == "content" || key == "output") {
      block.result = whole;
    }
  }

  void CommitBlock() {
    BlockCapture block = std::move(*block_);
    block_.reset();
    if (block.type == "text") {
      if (block.text.valid()) {
        out_.texts.push_back(block.text);
      }
      return;
    }
    if (block.type == "tool-call" || block.type == "tool_use") {
      if (block.name.valid()) {
        out_.tool_calls.push_back(block.name);
      }
      if (block.args.valid()) {
        out_.tool_calls.push_back(block.args);
      }
      return;
    }
    if (block.type == "tool-result" || block.type == "tool_result") {
      if (block.result.valid()) {
        out_.tool_results.push_back(block.result);
      }
      return;
    }
    // reasoning / thinking / unknown → not searchable.
  }

  std::string_view RangeView(Range r) const {
    return line_.substr(r.begin, r.end - r.begin);
  }

  std::string_view line_;
  std::vector<Frame> stack_;
  std::optional<BlockCapture> block_;
  LineCapture out_;
};

void AppendSpans(const std::vector<Range>& ranges,
                 uint64_t line_base,
                 uint32_t ordinal,
                 std::vector<TranscriptSpan>* out) {
  for (const Range& r : ranges) {
    out->push_back(TranscriptSpan{line_base + r.begin, line_base + r.end,
                                  ordinal});
  }
}

}  // namespace

TranscriptSpanIndex::TranscriptSpanIndex() = default;
TranscriptSpanIndex::TranscriptSpanIndex(TranscriptSpanIndex&&) noexcept =
    default;
TranscriptSpanIndex& TranscriptSpanIndex::operator=(
    TranscriptSpanIndex&&) noexcept = default;
TranscriptSpanIndex::~TranscriptSpanIndex() = default;

const std::vector<TranscriptSpan>& TranscriptSpanIndex::SpansFor(
    TranscriptSpanKind kind) const {
  switch (kind) {
    case TranscriptSpanKind::kUserText:
      return user_text;
    case TranscriptSpanKind::kAssistantText:
      return assistant_text;
    case TranscriptSpanKind::kSystemText:
      return system_text;
    case TranscriptSpanKind::kToolCall:
      return tool_call;
    case TranscriptSpanKind::kToolResult:
      return tool_result;
  }
  return user_text;
}

size_t TranscriptSpanIndex::ApproxBytes() const {
  size_t bytes = (user_text.size() + assistant_text.size() +
                  system_text.size() + tool_call.size() +
                  tool_result.size()) *
                 sizeof(TranscriptSpan);
  for (const TranscriptLineMeta& line : lines) {
    bytes += sizeof(TranscriptLineMeta) + line.uuid.size();
  }
  return bytes;
}

void IndexTranscriptChunk(std::string_view data, TranscriptSpanIndex* index) {
  size_t pos = 0;
  while (pos < data.size()) {
    const size_t newline = data.find('\n', pos);
    if (newline == std::string_view::npos) {
      // Trailing unterminated line: an append in flight. Leave it for
      // the next chunk — indexed_end stays at the last complete line.
      break;
    }
    size_t line_end = newline;
    if (line_end > pos && data[line_end - 1] == '\r') {
      --line_end;  // CRLF
    }
    const std::string_view line = data.substr(pos, line_end - pos);
    const uint64_t line_base = index->indexed_end + pos;
    const uint32_t ordinal = index->next_line_ordinal++;

    if (!line.empty()) {
      LineCapture capture = LineScanner(line).Scan();
      const bool is_message =
          capture.line_type == "user" || capture.line_type == "assistant";
      const bool is_system = capture.line_type == "system";
      if (!capture.malformed && is_system && !capture.is_meta &&
          capture.top_level_message.valid()) {
        index->system_text.push_back(TranscriptSpan{
            line_base + capture.top_level_message.begin,
            line_base + capture.top_level_message.end, ordinal});
        index->lines.push_back(TranscriptLineMeta{
            line_base, line_base + line.size(), ordinal,
            std::move(capture.uuid)});
      } else if (!capture.malformed && is_message && !capture.is_meta) {
        const bool is_user = capture.line_type == "user";
        std::vector<TranscriptSpan>* text_out =
            is_user ? &index->user_text : &index->assistant_text;
        if (capture.content_string.valid()) {
          text_out->push_back(
              TranscriptSpan{line_base + capture.content_string.begin,
                             line_base + capture.content_string.end, ordinal});
        }
        AppendSpans(capture.texts, line_base, ordinal, text_out);
        AppendSpans(capture.tool_calls, line_base, ordinal,
                    &index->tool_call);
        AppendSpans(capture.tool_results, line_base, ordinal,
                    &index->tool_result);
        index->lines.push_back(TranscriptLineMeta{
            line_base, line_base + line.size(), ordinal,
            std::move(capture.uuid)});
      }
    }
    pos = newline + 1;
  }
  index->indexed_end += pos;
}

}  // namespace nexus
