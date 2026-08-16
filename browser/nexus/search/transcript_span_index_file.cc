#include "custom_browser/browser/nexus/search/transcript_span_index_file.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_view_util.h"

namespace nexus {

namespace {

constexpr uint32_t kMagic = 0x49445356;  // "VSDI", little-endian
constexpr uint32_t kVersion = 1;

// Refuse anything absurd rather than allocating from a corrupt header.
constexpr uint32_t kMaxSpansPerKind = 4u * 1000 * 1000;
constexpr uint32_t kMaxLines = 4u * 1000 * 1000;
constexpr uint32_t kMaxUuidBytes = 256u * 1024 * 1024;

constexpr size_t kKindCount = 5;

// Fixed-width records. Written and read by the same binary, so layout
// only has to be stable within a build; `total_bytes` + the magic and
// version are what catch a foreign or half-written file.
struct DiskHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t source_size;
  int64_t source_mtime;  // base::Time internal microseconds
  uint64_t indexed_end;
  uint32_t next_line_ordinal;
  std::array<uint32_t, kKindCount> span_counts;
  uint32_t line_count;
  uint32_t uuid_bytes;
  uint64_t total_bytes;
};

struct DiskSpan {
  uint64_t begin;
  uint64_t end;
  uint32_t line_ordinal;
  uint32_t reserved;
};

struct DiskLine {
  uint64_t begin;
  uint64_t end;
  uint32_t ordinal;
  uint32_t uuid_offset;
  uint32_t uuid_length;
  uint32_t reserved;
};

std::vector<TranscriptSpan>* MutableSpansFor(TranscriptSpanIndex* index,
                                             size_t kind) {
  switch (kind) {
    case 0:
      return &index->user_text;
    case 1:
      return &index->assistant_text;
    case 2:
      return &index->system_text;
    case 3:
      return &index->tool_call;
    case 4:
      return &index->tool_result;
    default:
      return nullptr;
  }
}

const std::vector<TranscriptSpan>& SpansFor(const TranscriptSpanIndex& index,
                                            size_t kind) {
  static const std::vector<TranscriptSpan> kEmpty;
  switch (kind) {
    case 0:
      return index.user_text;
    case 1:
      return index.assistant_text;
    case 2:
      return index.system_text;
    case 3:
      return index.tool_call;
    case 4:
      return index.tool_result;
    default:
      return kEmpty;
  }
}

// Spans, not memcpy: Chromium builds with -Wunsafe-buffer-usage, and a
// serializer is exactly the place where raw pointer arithmetic goes
// wrong. `byte_span_from_ref` bounds every record to its own size.
template <typename T>
void AppendPod(std::string* out, const T& value) {
  out->append(base::as_string_view(base::byte_span_from_ref(value)));
}

// Copies one fixed-width record out of `bytes` at `offset`. Returns false
// when the file is too short for it — the caller then rejects the file.
template <typename T>
bool ReadPod(base::span<const uint8_t> bytes, size_t offset, T* value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
    return false;
  }
  base::byte_span_from_ref(*value).copy_from(
      bytes.subspan(offset, sizeof(T)));
  return true;
}

}  // namespace

base::FilePath SpanIndexFilePathFor(const base::FilePath& transcript) {
  return transcript.ReplaceExtension(FILE_PATH_LITERAL(".sdi"));
}

bool WriteSpanIndexFile(const base::FilePath& sidecar,
                        const TranscriptSpanIndex& index,
                        int64_t source_size,
                        base::Time source_mtime) {
  std::string uuids;
  std::vector<DiskLine> lines;
  lines.reserve(index.lines.size());
  for (const TranscriptLineMeta& line : index.lines) {
    DiskLine record = {};
    record.begin = line.line_begin;
    record.end = line.line_end;
    record.ordinal = line.line_ordinal;
    record.uuid_offset = base::checked_cast<uint32_t>(uuids.size());
    record.uuid_length = base::checked_cast<uint32_t>(line.uuid.size());
    uuids.append(line.uuid);
    lines.push_back(record);
  }

  DiskHeader header = {};
  header.magic = kMagic;
  header.version = kVersion;
  header.source_size = static_cast<uint64_t>(std::max<int64_t>(0, source_size));
  header.source_mtime =
      source_mtime.ToDeltaSinceWindowsEpoch().InMicroseconds();
  header.indexed_end = index.indexed_end;
  header.next_line_ordinal = index.next_line_ordinal;
  header.line_count = base::checked_cast<uint32_t>(lines.size());
  header.uuid_bytes = base::checked_cast<uint32_t>(uuids.size());

  size_t total = sizeof(DiskHeader);
  for (size_t kind = 0; kind < kKindCount; ++kind) {
    const size_t count = SpansFor(index, kind).size();
    header.span_counts[kind] = base::checked_cast<uint32_t>(count);
    total += count * sizeof(DiskSpan);
  }
  total += lines.size() * sizeof(DiskLine) + uuids.size();
  header.total_bytes = total;

  std::string blob;
  blob.reserve(total);
  AppendPod(&blob, header);
  for (size_t kind = 0; kind < kKindCount; ++kind) {
    for (const TranscriptSpan& span : SpansFor(index, kind)) {
      DiskSpan record = {};
      record.begin = span.begin;
      record.end = span.end;
      record.line_ordinal = span.line_ordinal;
      AppendPod(&blob, record);
    }
  }
  for (const DiskLine& line : lines) {
    AppendPod(&blob, line);
  }
  blob.append(uuids);

  // Write beside the target and rename over it: a search that reads the
  // sidecar while another one rewrites it must never see half a file.
  // (A torn file would still be rejected by the checks in Read, so this
  // is belt and braces — but a rebuild per crash is worth avoiding.)
  const base::FilePath temp = sidecar.AddExtension(FILE_PATH_LITERAL(".tmp"));
  if (!base::WriteFile(temp, blob)) {
    base::DeleteFile(temp);
    return false;
  }
  if (!base::ReplaceFile(temp, sidecar, nullptr)) {
    base::DeleteFile(temp);
    return false;
  }
  return true;
}

bool ReadSpanIndexFile(const base::FilePath& sidecar,
                       TranscriptSpanIndex* index,
                       int64_t* source_size,
                       base::Time* source_mtime) {
  std::string blob;
  if (!base::ReadFileToString(sidecar, &blob)) {
    return false;
  }
  const base::span<const uint8_t> bytes = base::as_byte_span(blob);
  DiskHeader header = {};
  if (!ReadPod(bytes, 0, &header)) {
    return false;
  }
  if (header.magic != kMagic || header.version != kVersion) {
    return false;
  }
  // A truncated write, or a file from a different build: the recorded
  // length is the cheapest way to notice before trusting the offsets.
  if (header.total_bytes != blob.size()) {
    return false;
  }
  if (header.line_count > kMaxLines || header.uuid_bytes > kMaxUuidBytes) {
    return false;
  }

  size_t offset = sizeof(DiskHeader);
  TranscriptSpanIndex loaded;
  for (size_t kind = 0; kind < kKindCount; ++kind) {
    const uint32_t count = header.span_counts[kind];
    if (count > kMaxSpansPerKind) {
      return false;
    }
    const size_t span_bytes = static_cast<size_t>(count) * sizeof(DiskSpan);
    std::vector<TranscriptSpan>* out = MutableSpansFor(&loaded, kind);
    if (!out) {
      return false;
    }
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      DiskSpan record = {};
      if (!ReadPod(bytes, offset + i * sizeof(DiskSpan), &record)) {
        return false;
      }
      out->push_back(
          TranscriptSpan{record.begin, record.end, record.line_ordinal});
    }
    offset += span_bytes;
  }

  const size_t line_bytes =
      static_cast<size_t>(header.line_count) * sizeof(DiskLine);
  if (offset + line_bytes + header.uuid_bytes > blob.size()) {
    return false;
  }
  const size_t uuid_base = offset + line_bytes;
  loaded.lines.reserve(header.line_count);
  for (uint32_t i = 0; i < header.line_count; ++i) {
    DiskLine record = {};
    if (!ReadPod(bytes, offset + i * sizeof(DiskLine), &record)) {
      return false;
    }
    if (static_cast<size_t>(record.uuid_offset) + record.uuid_length >
        header.uuid_bytes) {
      return false;
    }
    loaded.lines.push_back(TranscriptLineMeta{
        record.begin, record.end, record.ordinal,
        blob.substr(uuid_base + record.uuid_offset, record.uuid_length)});
  }

  loaded.indexed_end = header.indexed_end;
  loaded.next_line_ordinal = header.next_line_ordinal;

  *index = std::move(loaded);
  *source_size = static_cast<int64_t>(header.source_size);
  *source_mtime = base::Time::FromDeltaSinceWindowsEpoch(
      base::Microseconds(header.source_mtime));
  return true;
}

}  // namespace nexus
