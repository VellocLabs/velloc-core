#include "custom_browser/browser/nexus/tools/file_tool/file_tool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_media_service_launcher.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "net/base/mime_sniffer.h"
#include "net/base/mime_util.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"

namespace nexus {

// =============================================================================
// File-local helpers — kept anonymous so they don't pollute the
// nexus:: namespace and so we can iterate on them without touching the
// header. All UTF-8-safe; UTF-8 is self-synchronizing so byte-level
// substring search/replace is correct for any well-formed UTF-8 input.
// =============================================================================
namespace {

// CC rejects no-limit text reads when the total file exceeds 0.25 MiB,
// before reading. Explicit `limit` skips this total-file guard so targeted
// large-file reads remain possible. (CC parity: `maxSizeBytes` gates on TOTAL
// file size, pre-read — see tools/FileReadTool/limits.ts.)
constexpr int64_t kDefaultReadMaxSizeBytes = 256 * 1024;

// Read max output budget. CC parity (1-1-3 / 2-1-4): a FIXED 25000-token cap,
// independent of the active model's context window — CC's
// `DEFAULT_MAX_OUTPUT_TOKENS` does not scale. Nexus cannot call the model
// token-count API from native C++, so it approximates tokens by file type
// (EstimateReadTokens below). The prior context-window-scaled 12500/25000
// budget was removed to match the spec.
constexpr int kReadMaxOutputTokens = 25000;

constexpr size_t kMediaReadByteCap = 12 * 1024 * 1024;  // raw bytes

// CC parity (constants/apiLimits.ts `PDF_MAX_PAGES_PER_READ`): the most PDF
// pages a single Read may request. A range larger than this is rejected with
// errorCode 8 (spec 2-1-5).
constexpr int kPdfMaxPagesPerRead = 20;

constexpr char kFileUnchangedStub[] =
    "File unchanged since last read. The content from the earlier Read "
    "tool_result in this conversation is still current — refer to that instead "
    "of re-reading.";

std::string FormatOneDecimal(double value) {
  const int scaled = static_cast<int>(std::round(value * 10.0));
  const int whole = scaled / 10;
  const int fraction = scaled % 10;
  if (fraction == 0) {
    return base::NumberToString(whole);
  }
  return base::StrCat(
      {base::NumberToString(whole), ".", base::NumberToString(fraction)});
}

std::string FormatFileSize(int64_t size_in_bytes) {
  const double kb = static_cast<double>(size_in_bytes) / 1024.0;
  if (kb < 1.0) {
    return base::StrCat({base::NumberToString(size_in_bytes), " bytes"});
  }
  if (kb < 1024.0) {
    return base::StrCat({FormatOneDecimal(kb), "KB"});
  }
  const double mb = kb / 1024.0;
  if (mb < 1024.0) {
    return base::StrCat({FormatOneDecimal(mb), "MB"});
  }
  return base::StrCat({FormatOneDecimal(mb / 1024.0), "GB"});
}

int BytesPerTokenForFileType(std::string_view ext) {
  if (ext == "json" || ext == "jsonl" || ext == "jsonc") {
    return 2;
  }
  return 4;
}

int EstimateReadTokens(std::string_view content, std::string_view ext) {
  const double estimate = static_cast<double>(content.size()) /
                          static_cast<double>(BytesPerTokenForFileType(ext));
  if (estimate > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(std::llround(estimate));
}

std::string BuildFileTooLargeMessage(int64_t size_in_bytes) {
  return base::StrCat(
      {"File content (", FormatFileSize(size_in_bytes),
       ") exceeds maximum allowed size (",
       FormatFileSize(kDefaultReadMaxSizeBytes),
       "). Use offset and limit parameters to read specific portions of the "
       "file, "
       "or search for specific content instead of reading the whole file."});
}

struct ReadBudget {
  int max_output_tokens;
};

ReadBudget GetReadBudget() {
  return {kReadMaxOutputTokens};
}

std::string BuildTokenLimitMessage(int token_count, int max_output_tokens) {
  return base::StrCat(
      {"File content (", base::NumberToString(token_count),
       " tokens) exceeds maximum allowed tokens (",
       base::NumberToString(max_output_tokens),
       "). Use offset and limit parameters to read specific portions of the "
       "file, "
       "or search for specific content instead of reading the whole file."});
}

std::string LowercaseExtension(const base::FilePath& path) {
  std::string ext = base::FilePath(path.Extension()).AsUTF8Unsafe();
  if (!ext.empty() && ext.front() == '.') {
    ext.erase(ext.begin());
  }
  return base::ToLowerASCII(ext);
}

// CC parity (utils/pdfUtils.ts `parsePDFPageRange`): parse a PDF `pages`
// string into a (first, last) 1-indexed range. Supports "5", "1-10", and the
// open-ended "3-". Returns false on invalid input (empty, non-numeric, <1, or
// an inverted range). `last` is set to INT_MAX for the open-ended form.
bool ParsePdfPageRange(const std::string& pages, int* first, int* last) {
  const std::string trimmed(
      base::TrimWhitespaceASCII(pages, base::TRIM_ALL));
  if (trimmed.empty()) {
    return false;
  }
  if (trimmed.back() == '-') {
    int f = 0;
    if (!base::StringToInt(trimmed.substr(0, trimmed.size() - 1), &f) ||
        f < 1) {
      return false;
    }
    *first = f;
    *last = std::numeric_limits<int>::max();
    return true;
  }
  const size_t dash = trimmed.find('-');
  if (dash == std::string::npos) {
    int page = 0;
    if (!base::StringToInt(trimmed, &page) || page < 1) {
      return false;
    }
    *first = page;
    *last = page;
    return true;
  }
  int f = 0;
  int l = 0;
  if (!base::StringToInt(trimmed.substr(0, dash), &f) ||
      !base::StringToInt(trimmed.substr(dash + 1), &l) || f < 1 || l < 1 ||
      l < f) {
    return false;
  }
  *first = f;
  *last = l;
  return true;
}

// CC parity (FileReadTool.ts `isBlockedDevicePath` / `BLOCKED_DEVICE_PATHS`):
// device files that would hang the read (infinite output or blocking input).
// Path-only check, no I/O; safe devices like /dev/null are intentionally
// allowed. Nexus runs on a Windows host where these POSIX paths don't resolve,
// but the check is kept for spec parity and to guard a future POSIX host.
bool IsBlockedDevicePath(const std::string& path) {
  static constexpr std::string_view kBlocked[] = {
      "/dev/zero",   "/dev/random", "/dev/urandom", "/dev/full",
      "/dev/stdin",  "/dev/tty",    "/dev/console", "/dev/stdout",
      "/dev/stderr", "/dev/fd/0",   "/dev/fd/1",    "/dev/fd/2",
  };
  for (std::string_view blocked : kBlocked) {
    if (path == blocked) {
      return true;
    }
  }
  // /proc/self/fd/0-2 and /proc/<pid>/fd/0-2 are Linux aliases for stdio.
  if (base::StartsWith(path, "/proc/", base::CompareCase::SENSITIVE) &&
      (base::EndsWith(path, "/fd/0", base::CompareCase::SENSITIVE) ||
       base::EndsWith(path, "/fd/1", base::CompareCase::SENSITIVE) ||
       base::EndsWith(path, "/fd/2", base::CompareCase::SENSITIVE))) {
    return true;
  }
  return false;
}

bool IsSupportedImageMime(std::string_view mime) {
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" ||
         mime == "image/webp";
}

std::optional<std::string> ImageMimeFromPath(const base::FilePath& path) {
  std::string mime;
  if (net::GetMimeTypeFromFile(path, &mime) && IsSupportedImageMime(mime)) {
    return mime;
  }
  const std::string ext = LowercaseExtension(path);
  if (ext == "png") {
    return std::string("image/png");
  }
  if (ext == "jpg" || ext == "jpeg") {
    return std::string("image/jpeg");
  }
  if (ext == "gif") {
    return std::string("image/gif");
  }
  if (ext == "webp") {
    return std::string("image/webp");
  }
  return std::nullopt;
}

std::optional<std::string> SniffImageMime(base::span<const uint8_t> bytes) {
  std::string sniffed;
  std::string_view as_chars(reinterpret_cast<const char*>(bytes.data()),
                            bytes.size());
  if (net::SniffMimeTypeFromLocalData(as_chars, &sniffed) &&
      IsSupportedImageMime(sniffed)) {
    return sniffed;
  }
  return std::nullopt;
}

void AppendTextPart(base::ListValue* parts, std::string text) {
  if (text.empty()) {
    return;
  }
  base::DictValue part;
  part.Set("type", "text");
  part.Set("text", std::move(text));
  parts->Append(std::move(part));
}

void AppendImagePart(base::ListValue* parts,
                     std::string base64,
                     std::string mime_type) {
  base::DictValue part;
  part.Set("type", "image");
  part.Set("image", std::move(base64));
  part.Set("mimeType", std::move(mime_type));
  parts->Append(std::move(part));
}

void AppendDocumentPart(base::ListValue* parts,
                        std::string base64,
                        std::string mime_type) {
  base::DictValue part;
  part.Set("type", "document");
  part.Set("data", std::move(base64));
  part.Set("mimeType", std::move(mime_type));
  parts->Append(std::move(part));
}

FileTool::ReadResult MakeContentPartsResult(base::FilePath path,
                                            std::string message,
                                            base::ListValue parts,
                                            base::Time last_modified) {
  FileTool::ReadResult result;
  result.ok = true;
  result.file_path = std::move(path);
  result.last_modified = last_modified;
  result.payload.Set("message", std::move(message));
  result.payload.Set("content_parts", std::move(parts));
  return result;
}

std::optional<std::vector<uint8_t>> ReadBytesWithCap(
    const base::FilePath& path,
    FileTool::ReadResult* result) {
  base::File::Info info;
  if (base::GetFileInfo(path, &info) && info.size > 0 &&
      static_cast<uint64_t>(info.size) > kMediaReadByteCap) {
    result->error_code = "file_too_large";
    result->error_message = base::StrCat(
        {"file is too large to read as media: ", path.AsUTF8Unsafe()});
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(path);
  if (!bytes.has_value()) {
    result->error_code = "read_failed";
    result->error_message =
        base::StrCat({"failed to read file: ", path.AsUTF8Unsafe()});
    return std::nullopt;
  }
  if (bytes->size() > kMediaReadByteCap) {
    result->error_code = "file_too_large";
    result->error_message = base::StrCat(
        {"file is too large to read as media: ", path.AsUTF8Unsafe()});
    return std::nullopt;
  }
  return bytes;
}

std::string ReadTextFromValue(const base::Value* value) {
  if (!value) {
    return std::string();
  }
  if (value->is_string()) {
    return value->GetString();
  }
  const base::ListValue* list = value->GetIfList();
  if (!list) {
    return std::string();
  }
  std::string out;
  for (const auto& item : *list) {
    if (item.is_string()) {
      out.append(item.GetString());
    }
  }
  return out;
}

std::string TruncateNotebookOutput(std::string text) {
  constexpr size_t kMaxNotebookOutputBytes = 10000;
  if (text.size() <= kMaxNotebookOutputBytes) {
    return text;
  }
  text.resize(kMaxNotebookOutputBytes);
  text.append("\n[output truncated]");
  return text;
}

std::string NotebookCellId(const base::DictValue& cell, int index) {
  const std::string* id = cell.FindString("id");
  if (id && !id->empty()) {
    return *id;
  }
  return base::StrCat({"cell-", base::NumberToString(index)});
}

void AppendNotebookOutputParts(const base::DictValue& output,
                               base::ListValue* parts) {
  const std::string* output_type = output.FindString("output_type");
  if (!output_type) {
    return;
  }

  if (*output_type == "stream") {
    AppendTextPart(parts, "\n" + TruncateNotebookOutput(
                                     ReadTextFromValue(output.Find("text"))));
    return;
  }

  if (*output_type == "error") {
    std::string text;
    if (const std::string* ename = output.FindString("ename")) {
      text.append(*ename);
    }
    if (const std::string* evalue = output.FindString("evalue")) {
      if (!text.empty()) {
        text.append(": ");
      }
      text.append(*evalue);
    }
    if (const base::ListValue* traceback = output.FindList("traceback")) {
      for (const auto& line : *traceback) {
        if (line.is_string()) {
          text.push_back('\n');
          text.append(line.GetString());
        }
      }
    }
    AppendTextPart(parts, "\n" + TruncateNotebookOutput(std::move(text)));
    return;
  }

  if (*output_type != "execute_result" && *output_type != "display_data") {
    return;
  }

  const base::DictValue* data = output.FindDict("data");
  if (!data) {
    return;
  }
  AppendTextPart(parts, "\n" + TruncateNotebookOutput(ReadTextFromValue(
                                   data->Find("text/plain"))));
  if (const std::string* png = data->FindString("image/png")) {
    std::string compact;
    base::RemoveChars(*png, " \t\r\n", &compact);
    AppendImagePart(parts, std::move(compact), "image/png");
  } else if (const std::string* jpeg = data->FindString("image/jpeg")) {
    std::string compact;
    base::RemoveChars(*jpeg, " \t\r\n", &compact);
    AppendImagePart(parts, std::move(compact), "image/jpeg");
  }
}

FileTool::ReadResult ReadNotebookFile(const base::FilePath& path,
                                      const std::string& contents,
                                      base::Time last_modified) {
  FileTool::ReadResult result;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    result.error_code = "invalid_notebook";
    result.error_message = "failed to parse notebook JSON";
    return result;
  }

  const base::DictValue& notebook = parsed->GetDict();
  const base::ListValue* cells = notebook.FindList("cells");
  if (!cells) {
    result.error_code = "invalid_notebook";
    result.error_message = "notebook JSON does not contain a cells array";
    return result;
  }

  std::string language = "python";
  if (const base::DictValue* metadata = notebook.FindDict("metadata")) {
    if (const base::DictValue* language_info =
            metadata->FindDict("language_info")) {
      if (const std::string* name = language_info->FindString("name")) {
        language = *name;
      }
    }
  }

  base::ListValue parts;
  int index = 0;
  for (const auto& cell_value : *cells) {
    const base::DictValue* cell = cell_value.GetIfDict();
    if (!cell) {
      ++index;
      continue;
    }
    const std::string* cell_type = cell->FindString("cell_type");
    const std::string type = cell_type ? *cell_type : std::string("code");
    const std::string source = ReadTextFromValue(cell->Find("source"));
    const std::string id = NotebookCellId(*cell, index);

    std::string cell_text = base::StrCat({"<cell id=\"", id, "\">"});
    if (type != "code") {
      cell_text.append(base::StrCat({"<cell_type>", type, "</cell_type>"}));
    } else if (language != "python") {
      cell_text.append(base::StrCat({"<language>", language, "</language>"}));
    }
    cell_text.append(source);
    cell_text.append(base::StrCat({"</cell id=\"", id, "\">"}));
    AppendTextPart(&parts, std::move(cell_text));

    if (type == "code") {
      if (const base::ListValue* outputs = cell->FindList("outputs")) {
        for (const auto& output_value : *outputs) {
          if (const base::DictValue* output = output_value.GetIfDict()) {
            AppendNotebookOutputParts(*output, &parts);
          }
        }
      }
    }
    ++index;
  }

  result = MakeContentPartsResult(
      path,
      base::StrCat({"Notebook read: ", path.AsUTF8Unsafe(), " (",
                    base::NumberToString(cells->size()), " cells)"}),
      std::move(parts), last_modified);
  result.full_content = contents;
  result.cache_read_state = true;
  return result;
}

struct EncodedImage {
  std::vector<uint8_t> bytes;
  std::string mime_type;
};

std::optional<SkBitmap> DecodeImageForCompression(
    base::span<const uint8_t> bytes,
    std::string_view mime_type) {
  if (mime_type == "image/png") {
    SkBitmap bitmap = gfx::PNGCodec::Decode(bytes);
    if (!bitmap.drawsNothing()) {
      return bitmap;
    }
  }
  if (mime_type == "image/jpeg") {
    SkBitmap bitmap = gfx::JPEGCodec::Decode(bytes);
    if (!bitmap.drawsNothing()) {
      return bitmap;
    }
  }
  return std::nullopt;
}

EncodedImage MaybeCompressImageForRead(const std::vector<uint8_t>& bytes,
                                       const std::string& mime_type) {
  constexpr size_t kImageCompressionThreshold = 256 * 1024;
  constexpr int kMaxImageDimension = 1600;
  if (bytes.size() <= kImageCompressionThreshold) {
    return EncodedImage{bytes, mime_type};
  }

  std::optional<SkBitmap> decoded =
      DecodeImageForCompression(base::span(bytes), mime_type);
  if (!decoded.has_value()) {
    return EncodedImage{bytes, mime_type};
  }

  SkBitmap bitmap = std::move(*decoded);
  SkBitmap* encode_bitmap = &bitmap;
  SkBitmap scaled;
  const int max_dimension = std::max(bitmap.width(), bitmap.height());
  if (max_dimension > kMaxImageDimension) {
    const double scale =
        static_cast<double>(kMaxImageDimension) / max_dimension;
    const int width =
        std::max(1, static_cast<int>(std::floor(bitmap.width() * scale)));
    const int height =
        std::max(1, static_cast<int>(std::floor(bitmap.height() * scale)));
    if (scaled.tryAllocN32Pixels(width, height)) {
      SkPixmap dst;
      if (scaled.peekPixels(&dst) &&
          bitmap.pixmap().scalePixels(
              dst, SkSamplingOptions(SkFilterMode::kLinear))) {
        encode_bitmap = &scaled;
      }
    }
  }

  std::optional<std::vector<uint8_t>> jpeg =
      gfx::JPEGCodec::Encode(*encode_bitmap, 82);
  if (jpeg.has_value() && !jpeg->empty() && jpeg->size() < bytes.size()) {
    return EncodedImage{std::move(*jpeg), "image/jpeg"};
  }
  return EncodedImage{bytes, mime_type};
}

FileTool::ReadResult ReadImageFile(const base::FilePath& path,
                                   const std::vector<uint8_t>& bytes,
                                   std::string mime_type,
                                   base::Time last_modified) {
  EncodedImage encoded = MaybeCompressImageForRead(bytes, mime_type);
  base::ListValue parts;
  AppendTextPart(&parts,
                 base::StrCat({"Image file read: ", path.AsUTF8Unsafe(), " (",
                               base::NumberToString(bytes.size()), " bytes)"}));
  AppendImagePart(&parts, base::Base64Encode(encoded.bytes),
                  std::move(encoded.mime_type));
  return MakeContentPartsResult(
      path, base::StrCat({"Image file read: ", path.AsUTF8Unsafe()}),
      std::move(parts), last_modified);
}

// Build the "1\tline\n2\tline\n…" projection that Claude Code's Read
// tool returns. The model is trained on this exact shape, so matching
// it minimizes prompt drift even though it's a presentation choice.
//
// `start_line_number` is the 1-indexed number to print on the FIRST
// emitted line (typically `offset` if specified, else 1). When
// `max_lines` has a value, the function stops after that many lines
// (the caller-supplied `limit`); when it is nullopt the whole file from
// `start` is emitted. CC parity (2-1-9 / 2-1-10): there is NO default
// line cap — `MAX_LINES_TO_READ=2000` is prompt-only — and NO byte cap;
// overflow is enforced by the post-read token guard, which THROWS rather
// than silently truncating.
//
// `content` is the same selected text without line-number prefixes, used for
// CC-style output token validation.
struct ProjectionResult {
  std::string projection;
  std::string content;
  int lines_emitted = 0;
  // Total number of lines in the file (after stripping the phantom
  // trailing entry of a "\n"-terminated file). Used by the caller to
  // emit CC's offset-past-EOF warning, which reports the file length.
  int total_lines = 0;
  bool truncated = false;
};

ProjectionResult BuildLineNumberedProjection(std::string_view contents,
                                             int start_line_number,
                                             std::optional<int> max_lines) {
  ProjectionResult out;

  // SplitStringPiece with KEEP_WHITESPACE preserves blank lines so the
  // emitted line numbers remain in lock-step with what an editor would
  // show. SPLIT_WANT_ALL keeps trailing empty splits (a file ending in
  // "\n" produces an extra empty line we DON'T want to emit, so we
  // strip the final empty entry below).
  std::vector<std::string_view> lines = base::SplitStringPiece(
      contents, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);

  // Trim trailing empty entry from a "...\n"-terminated file so we
  // don't emit a phantom blank line at the end.
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  out.total_lines = static_cast<int>(lines.size());

  // start_line_number is 1-indexed; convert to a 0-indexed slice
  // start. We treat any out-of-range start as "no lines" — the
  // payload will simply have an empty projection, which the LLM
  // can detect via lines_emitted=0.
  size_t start =
      (start_line_number > 0) ? static_cast<size_t>(start_line_number - 1) : 0;
  if (start >= lines.size()) {
    return out;  // empty projection, lines_emitted=0
  }

  // Emit from `start`, honoring the caller's explicit `limit` when set
  // (no default line cap, no byte cap — CC parity).
  out.projection.reserve(contents.size() + 16 * lines.size());
  size_t emitted = 0;
  for (size_t i = start; i < lines.size(); ++i) {
    if (max_lines.has_value() &&
        emitted >= static_cast<size_t>(*max_lines)) {
      break;
    }
    std::string line(lines[i]);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::string prefix = base::NumberToString(static_cast<int>(i) + 1) + "\t";
    if (!out.content.empty()) {
      out.content.push_back('\n');
    }
    out.content.append(line);
    out.projection.append(prefix);
    out.projection.append(line);
    out.projection.push_back('\n');
    ++emitted;
  }
  out.lines_emitted = static_cast<int>(emitted);

  // If we stopped because of the explicit line cap but more lines remain
  // in the file, mark truncated so the LLM knows to ask for a follow-up
  // `read` with a larger offset.
  if (!out.truncated && start + emitted < lines.size()) {
    out.truncated = true;
  }
  return out;
}

// Count occurrences of `needle` in `haystack` without overlapping
// matches. Returns 0 for an empty needle (which the caller already
// rejects, but defensive against future refactors).
size_t CountOccurrences(const std::string& haystack,
                        const std::string& needle) {
  if (needle.empty()) {
    return 0;
  }
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// =============================================================================
// Edit helpers — CC FileEditTool/utils.ts parity
// =============================================================================

// CC's MAX_EDIT_FILE_SIZE: 1 GiB (stat bytes). Prevents OOM on multi-GB files.
constexpr int64_t kMaxEditFileSize = 1024 * 1024 * 1024;

// Detect whether content uses CRLF line endings.
bool HasCRLF(const std::string& content) {
  return content.find("\r\n") != std::string::npos;
}

// Normalize CRLF → LF (CC's readFileSyncWithMetadata does this before
// matching). Standalone \r is left untouched (CC only strips \r\n pairs).
std::string NormalizeCRLF(const std::string& content) {
  std::string result;
  result.reserve(content.size());
  for (size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r' && i + 1 < content.size() &&
        content[i + 1] == '\n') {
      result.push_back('\n');
      ++i;
    } else {
      result.push_back(content[i]);
    }
  }
  return result;
}

// Restore LF → CRLF (write back with original line endings).
std::string RestoreCRLF(const std::string& content) {
  std::string result;
  result.reserve(content.size() + content.size() / 16);
  for (size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\n' && (i == 0 || content[i - 1] != '\r')) {
      result.push_back('\r');
    }
    result.push_back(content[i]);
  }
  return result;
}

// Detect BOM-based encoding from raw file bytes and re-encode UTF-8
// content to match. Mirrors CC's detectEncodingForResolvedPath
// (utils/fileRead.ts:20-49) + writeTextContent encoding passthrough.
// Returns the bytes to write (may differ from |content| if transcoded).
std::string PreserveOriginalEncoding(const std::string& original,
                                     const std::string& content) {
  // UTF-16LE BOM: FF FE
  if (original.size() >= 2 &&
      static_cast<unsigned char>(original[0]) == 0xFF &&
      static_cast<unsigned char>(original[1]) == 0xFE) {
    std::u16string utf16 = base::UTF8ToUTF16(content);
    std::string result;
    result.reserve(2 + utf16.size() * 2);
    result.push_back('\xFF');
    result.push_back('\xFE');
    for (char16_t c : utf16) {
      result.push_back(static_cast<char>(c & 0xFF));
      result.push_back(static_cast<char>((c >> 8) & 0xFF));
    }
    return result;
  }
  // UTF-8 BOM: EF BB BF
  if (original.size() >= 3 &&
      static_cast<unsigned char>(original[0]) == 0xEF &&
      static_cast<unsigned char>(original[1]) == 0xBB &&
      static_cast<unsigned char>(original[2]) == 0xBF) {
    return "\xEF\xBB\xBF" + content;
  }
  return content;
}

// Decode raw on-disk bytes to UTF-8 for text projection/matching. Mirrors CC's
// readFileSyncWithMetadata encoding detection (utils/fileRead.ts:20-49): a
// UTF-16LE BOM (FF FE) triggers a transcode to UTF-8; anything else is treated
// as UTF-8 and returned unchanged. `is_utf16le` reports whether a transcode
// happened so callers can (a) skip the binary heuristic for UTF-16LE (its null
// bytes would otherwise trip LooksLikeBinary) and (b) re-encode on write via
// PreserveOriginalEncoding. The read_state_ cache keeps the RAW bytes so the
// Edit/Write staleness check (raw-vs-raw) stays symmetric with Read.
std::string DecodeToUTF8(const std::string& raw, bool* is_utf16le) {
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
      static_cast<unsigned char>(raw[1]) == 0xFE) {
    *is_utf16le = true;
    // Bytes after the 2-byte BOM are little-endian UTF-16 code units.
    std::u16string utf16;
    utf16.reserve((raw.size() - 2) / 2);
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
      utf16.push_back(static_cast<char16_t>(
          static_cast<unsigned char>(raw[i]) |
          (static_cast<unsigned char>(raw[i + 1]) << 8)));
    }
    return base::UTF16ToUTF8(utf16);
  }
  *is_utf16le = false;
  return raw;
}

// CC utils.ts normalizeQuotes: convert curly quotes to straight.
// UTF-8: U+2018 '=E2 80 98, U+2019 '=E2 80 99,
//        U+201C "=E2 80 9C, U+201D "=E2 80 9D.
std::string NormalizeQuotes(const std::string& str) {
  std::string result;
  result.reserve(str.size());
  for (size_t i = 0; i < str.size();) {
    if (i + 2 < str.size() &&
        static_cast<unsigned char>(str[i]) == 0xE2 &&
        static_cast<unsigned char>(str[i + 1]) == 0x80) {
      const unsigned char c = static_cast<unsigned char>(str[i + 2]);
      if (c == 0x98 || c == 0x99) {
        result.push_back('\'');
        i += 3;
        continue;
      }
      if (c == 0x9C || c == 0x9D) {
        result.push_back('"');
        i += 3;
        continue;
      }
    }
    result.push_back(str[i]);
    ++i;
  }
  return result;
}

// CC utils.ts findActualString: exact match first, then quote-normalized.
// Returns the actual substring from file_content that matches search_string.
std::string FindActualString(const std::string& file_content,
                             const std::string& search_string,
                             bool* found) {
  if (file_content.find(search_string) != std::string::npos) {
    *found = true;
    return search_string;
  }
  const std::string normalized_search = NormalizeQuotes(search_string);
  const std::string normalized_file = NormalizeQuotes(file_content);
  const size_t pos = normalized_file.find(normalized_search);
  if (pos != std::string::npos) {
    *found = true;
    return file_content.substr(pos, search_string.size());
  }
  *found = false;
  return std::string();
}

// CC utils.ts isOpeningContext heuristic for curly quote direction.
bool IsOpeningContext(const std::string& str, size_t byte_pos) {
  if (byte_pos == 0) {
    return true;
  }
  const char prev = str[byte_pos - 1];
  return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
         prev == '(' || prev == '[' || prev == '{';
}

// CC utils.ts preserveQuoteStyle: when old_string matched via quote
// normalization, apply the same curly quote style to new_string.
std::string PreserveQuoteStyle(const std::string& old_string,
                               const std::string& actual_old_string,
                               const std::string& new_string) {
  if (old_string == actual_old_string) {
    return new_string;
  }
  const bool has_double =
      actual_old_string.find("\xE2\x80\x9C") != std::string::npos ||
      actual_old_string.find("\xE2\x80\x9D") != std::string::npos;
  const bool has_single =
      actual_old_string.find("\xE2\x80\x98") != std::string::npos ||
      actual_old_string.find("\xE2\x80\x99") != std::string::npos;
  if (!has_double && !has_single) {
    return new_string;
  }

  std::string result;
  result.reserve(new_string.size() + 16);
  for (size_t i = 0; i < new_string.size(); ++i) {
    if (new_string[i] == '"' && has_double) {
      result.append(IsOpeningContext(new_string, i) ? "\xE2\x80\x9C"
                                                    : "\xE2\x80\x9D");
    } else if (new_string[i] == '\'' && has_single) {
      const bool prev_is_letter =
          i > 0 && (base::IsAsciiAlpha(new_string[i - 1]) ||
                    static_cast<unsigned char>(new_string[i - 1]) > 0x7F);
      const bool next_is_letter =
          i + 1 < new_string.size() &&
          (base::IsAsciiAlpha(new_string[i + 1]) ||
           static_cast<unsigned char>(new_string[i + 1]) > 0x7F);
      if (prev_is_letter && next_is_letter) {
        result.append("\xE2\x80\x99");
      } else {
        result.append(IsOpeningContext(new_string, i) ? "\xE2\x80\x98"
                                                      : "\xE2\x80\x99");
      }
    } else {
      result.push_back(new_string[i]);
    }
  }
  return result;
}

// CC utils.ts stripTrailingWhitespace: strip trailing whitespace per line.
std::string StripTrailingWhitespace(const std::string& str) {
  std::string result;
  result.reserve(str.size());
  size_t line_start = 0;
  for (size_t i = 0; i <= str.size(); ++i) {
    if (i == str.size() || str[i] == '\n') {
      size_t end = i;
      while (end > line_start &&
             (str[end - 1] == ' ' || str[end - 1] == '\t' ||
              str[end - 1] == '\r')) {
        --end;
      }
      result.append(str, line_start, end - line_start);
      if (i < str.size()) {
        result.push_back('\n');
      }
      line_start = i + 1;
    }
  }
  return result;
}

// CC utils.ts DESANITIZATIONS: the model can't see certain XML-like tokens
// (they're sanitized in the API), so it outputs the sanitized versions.
// Map sanitized → real so edits match the actual file content.
struct DesanitizationEntry {
  const char* from;
  const char* to;
};
constexpr DesanitizationEntry kDesanitizations[] = {
    {"<fnr>", "<function_results>"},
    {"<n>", "<name>"},
    {"</n>", "</name>"},
    {"<o>", "<output>"},
    {"</o>", "</output>"},
    {"<e>", "<error>"},
    {"</e>", "</error>"},
    {"<s>", "<system>"},
    {"</s>", "</system>"},
    {"<r>", "<result>"},
    {"</r>", "</result>"},
    {"< META_START >", "<META_START>"},
    {"< META_END >", "<META_END>"},
    {"< EOT >", "<EOT>"},
    {"< META >", "<META>"},
    {"< SOS >", "<SOS>"},
    {"\n\nH:", "\n\nHuman:"},
    {"\n\nA:", "\n\nAssistant:"},
};

// Apply desanitization replacements to a string. Returns the result and
// records which replacements were applied (for mirroring onto new_string).
std::string DesanitizeString(
    const std::string& input,
    std::vector<std::pair<std::string, std::string>>* applied) {
  std::string result = input;
  for (const auto& entry : kDesanitizations) {
    const std::string from(entry.from);
    const std::string to(entry.to);
    if (result.find(from) != std::string::npos) {
      base::ReplaceSubstringsAfterOffset(&result, 0, from, to);
      if (applied) {
        applied->emplace_back(from, to);
      }
    }
  }
  return result;
}

// CC utils.ts applyEditToFile: replace with delete-strips-newline logic.
// When new_string is empty (deletion) and old_string doesn't end with \n
// but is followed by \n in the file, the trailing newline is also removed.
std::string ApplyEditToFile(const std::string& content,
                            const std::string& old_string,
                            const std::string& new_string,
                            bool replace_all) {
  if (old_string.empty()) {
    return new_string;  // file creation
  }
  if (!new_string.empty()) {
    if (replace_all) {
      std::string result = content;
      base::ReplaceSubstringsAfterOffset(&result, 0, old_string, new_string);
      return result;
    }
    std::string result = content;
    const size_t pos = result.find(old_string);
    if (pos == std::string::npos) {
      return content;
    }
    result.replace(pos, old_string.size(), new_string);
    return result;
  }
  // Deletion: strip trailing newline if old_string is followed by \n.
  const bool strip_trailing_newline =
      (old_string.back() != '\n') &&
      content.find(old_string + "\n") != std::string::npos;
  const std::string target =
      strip_trailing_newline ? (old_string + "\n") : old_string;
  if (replace_all) {
    std::string result = content;
    base::ReplaceSubstringsAfterOffset(&result, 0, target, "");
    return result;
  }
  std::string result = content;
  const size_t pos = result.find(target);
  if (pos == std::string::npos) {
    return content;
  }
  result.erase(pos, target.size());
  return result;
}

// =============================================================================
// Glob helpers
// =============================================================================

// Cap for Glob: keep the agent's response budget bounded.
constexpr int kDefaultGlobLimit = 100;

// Wall-clock budget for the `rg --files` exec. Generous (the shell service's
// own ceiling) so a `--sort=modified` (single-threaded) walk of a large
// `--no-ignore` tree completes instead of being killed — still far under the
// 180s action budget that the old FileEnumerator walk overran.
constexpr int kGlobTimeoutMs = 120000;

// Byte cap on rg's captured stdout. A glob listing many files can be large;
// 4 MiB holds tens of thousands of paths before we slice to `limit`.
constexpr int kGlobMaxOutputBytes = 4 * 1024 * 1024;

// Resolve `path` against `cwd_or_default`: absolute paths used as-is,
// relative paths joined onto the default. Empty `path` returns the
// default. Returns base::FilePath() if both inputs are empty; the
// caller treats that as a "no usable cwd" error.
base::FilePath ResolveAgainstCwd(const std::string& path,
                                 const base::FilePath& default_root) {
  if (path.empty()) {
    return default_root;
  }
  base::FilePath candidate = base::FilePath::FromUTF8Unsafe(path);
  if (candidate.IsAbsolute()) {
    return candidate;
  }
  if (default_root.empty()) {
    return base::FilePath();
  }
  return default_root.Append(candidate);
}

}  // namespace

// =============================================================================
// Out-of-line ctors/dtors for the per-method result structs.
//
// Required by chromium-style: any class/struct with non-trivial member
// types (std::string, base::Value::Dict) must declare AND define its
// special members out-of-line. Defining them here as `= default` keeps
// them trivial in spirit while satisfying the clang plugin and giving
// the compiler a single, predictable instantiation point.
//
// All three structs are move-only: base::Value::Dict is move-only, and
// allowing copies would silently regress that. `noexcept` on the move
// ops matters for std::vector / std::optional / base::BindOnce
// performance — they pick the move path only when it can't throw.
// =============================================================================

FileTool::ReadResult::ReadResult() = default;
FileTool::ReadResult::ReadResult(ReadResult&&) noexcept = default;
FileTool::ReadResult& FileTool::ReadResult::operator=(ReadResult&&) noexcept =
    default;
FileTool::ReadResult::~ReadResult() = default;

FileTool::WriteResult::WriteResult() = default;
FileTool::WriteResult::WriteResult(WriteResult&&) noexcept = default;
FileTool::WriteResult& FileTool::WriteResult::operator=(
    WriteResult&&) noexcept = default;
FileTool::WriteResult::~WriteResult() = default;

FileTool::EditResult::EditResult() = default;
FileTool::EditResult::EditResult(EditResult&&) noexcept = default;
FileTool::EditResult& FileTool::EditResult::operator=(EditResult&&) noexcept =
    default;
FileTool::EditResult::~EditResult() = default;

// =============================================================================
// Construction / lifecycle
// =============================================================================

FileTool::FileTool(ToolSession* session,
                   Profile* profile,
                   const std::string& session_key)
    : ToolBase(session, profile, session_key),
      launcher_(std::make_unique<NexusShellServiceLauncher>()),
      media_launcher_(std::make_unique<NexusMediaServiceLauncher>()) {
  // No cached WeakPtr needed: the Run<Name>OnUI bodies run on UI (the
  // dispatcher invokes them synchronously) so they call
  // `weak_ptr_factory_.GetWeakPtr()` directly to bind each reply —
  // same sequence as the factory, like ProcessTool / BrowserTool.
  //
  // `launcher_` backs the Glob tool's `rg` exec via `ripgrep_runner_`;
  // Read/Write/Edit don't use it (direct thread-pool file I/O).
}

FileTool::~FileTool() {
  // Defensive — invalidate WeakPtrs so any reply still queued on
  // ThreadPool drops cleanly. OnStop is idempotent so the explicit
  // call here is safe even if it already ran.
  OnStop(OnStopReason::kDestructor);
}

void FileTool::OnStop(OnStopReason reason) {
  LOG(INFO) << "[FileTool] OnStop session=" << GetSessionKey()
            << " reason=" << OnStopReasonToString(reason);
  // InvalidateWeakPtrs is gated on TEARDOWN reasons only — the ones
  // that mean this FileTool is actually going away.
  //
  // Each Run<Name>OnUI binds its PostTaskAndReplyWithResult reply
  // through `weak_ptr_factory_.GetWeakPtr()`. On a genuine teardown
  // (session deleted / panel closed / shutdown / dtor) we invalidate so
  // any reply whose blocking I/O is still in flight on the thread pool
  // drops cleanly — the discarded ToolCallback emits `tool_dropped`
  // rather than resolving into a half-destroyed FileTool.
  //
  // Per-turn cleanup reasons (kCompleted, kUserStopped) fire OnStop
  // while the ToolSession stays warm in the adapter map (typed-OnStop
  // refactor — see tools_adapter.cc). The agent loop blocks on tool
  // results before declaring a turn complete, so there is no in-flight
  // file I/O to cancel at those points; invalidating would be pure
  // churn (and `GetWeakPtr()` simply re-vends a fresh, valid WeakPtr on
  // the next turn's call anyway). Skip it.
  const bool teardown = reason == OnStopReason::kSessionDeleted ||
                        reason == OnStopReason::kDisconnected ||
                        reason == OnStopReason::kShutdown ||
                        reason == OnStopReason::kDestructor;
  if (teardown) {
    weak_ptr_factory_.InvalidateWeakPtrs();
    // Drop any in-flight Glob rg exec / auto-install callback before the
    // launcher it borrows is torn down.
    ripgrep_runner_.Reset();
    if (launcher_) {
      launcher_->Shutdown();
    }
    if (media_launcher_) {
      media_launcher_->Shutdown();
    }
  }
}

void FileTool::ClearReadState() {
  read_state_.clear();
}

// =============================================================================
// Run<Name>OnUI overrides — invoked directly on UI by the codegen
// dispatcher (the JSON declarations carry `runtime.thread = "ui"`).
//
// Each body runs on UI, so:
//   - it may read FileTool / ToolSession state safely (`GetSession()`,
//     `weak_ptr_factory_.GetWeakPtr()`) — the object is guaranteed
//     alive on the dispatcher's own sequence;
//   - it validates cheaply, then hands the BLOCKING file I/O to a
//     base::ThreadPool worker via PostTaskAndReplyWithResult. The work
//     is a *static* `<Name>Blocking` helper (no `this`), so a session
//     torn down while the I/O is in flight can never be dereferenced by
//     the worker;
//   - the reply `On<Name>Done` is bound through GetWeakPtr() and runs
//     back on UI. If the session ended meanwhile, the WeakPtr is
//     invalidated and the reply drops; otherwise it does the single
//     `done.Resolve` / `done.Reject`. Resolve/Reject must run on UI
//     because the callbacks inside `done` hold a UI-resident
//     `WeakPtr<ToolsAdapter>`.
//
// Validation failures reject `done` inline — we're already on UI, so
// there's no need to round-trip through the reply trampoline.
//
// The hop traits are {MayBlock(), USER_VISIBLE}: disk I/O may block,
// and the user is waiting on the LLM's tool result (not background
// bookkeeping). Inlined at each call site to match BrowserTool's
// download path and ProcessTool.
// =============================================================================

void FileTool::RunReadOnUI(::nexus::generated::ReadInput input,
                           ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    std::move(done).Reject("invalid_args",
                           "file_path must be an absolute path");
    return;
  }

  // CC parity (FileReadTool.validateInput, spec 2-1-5): validate the `pages`
  // parameter BEFORE any I/O. Bad format → errorCode 7; a range wider than
  // kPdfMaxPagesPerRead → errorCode 8. The message wording mirrors CC so the
  // model's correction pattern is shared.
  if (input.pages.has_value()) {
    int first_page = 0;
    int last_page = 0;
    if (!ParsePdfPageRange(*input.pages, &first_page, &last_page)) {
      std::move(done).Reject(
          "invalid_pages",
          base::StrCat({"Invalid pages parameter: \"", *input.pages,
                        "\". Use formats like \"1-5\", \"3\", or \"10-20\". "
                        "Pages are 1-indexed."}));
      return;
    }
    const int range_size =
        last_page == std::numeric_limits<int>::max()
            ? kPdfMaxPagesPerRead + 1
            : last_page - first_page + 1;
    if (range_size > kPdfMaxPagesPerRead) {
      std::move(done).Reject(
          "too_many_pages",
          base::StrCat({"Page range \"", *input.pages,
                        "\" exceeds maximum of ",
                        base::NumberToString(kPdfMaxPagesPerRead),
                        " pages per request. Please use a smaller range."}));
      return;
    }
  }

  // CC parity (FileReadTool.validateInput `isBlockedDevicePath`, spec 2-1-5
  // errorCode 9): a path-based check (no I/O) for device files that would hang
  // the read. Safe devices like /dev/null are intentionally allowed.
  if (IsBlockedDevicePath(input.file_path)) {
    std::move(done).Reject(
        "blocked_device_path",
        base::StrCat({"Cannot read '", input.file_path,
                      "': this device file would block or produce infinite "
                      "output."}));
    return;
  }

  const std::string ext = LowercaseExtension(path);
  if (ext == "pdf" || ext == "ipynb" || ImageMimeFromPath(path).has_value()) {
    auto options = mojom::MediaReadOptions::New();
    options->path = path;
    if (ext == "pdf") {
      options->pages = input.pages;
    }
    options->max_bytes = static_cast<uint32_t>(kMediaReadByteCap);
    if (std::optional<std::string> image_mime = ImageMimeFromPath(path)) {
      options->mime_type = *image_mime;
      media_launcher_->GetService()->ReadImage(
          std::move(options), base::BindOnce(&FileTool::OnReadMediaDone,
                                             weak_ptr_factory_.GetWeakPtr(),
                                             path, std::move(done)));
    } else if (ext == "ipynb") {
      media_launcher_->GetService()->ReadNotebook(
          std::move(options), base::BindOnce(&FileTool::OnReadMediaDone,
                                             weak_ptr_factory_.GetWeakPtr(),
                                             path, std::move(done)));
    } else {
      media_launcher_->GetService()->ReadPdf(
          std::move(options), base::BindOnce(&FileTool::OnReadMediaDone,
                                             weak_ptr_factory_.GetWeakPtr(),
                                             path, std::move(done)));
    }
    return;
  }

  std::optional<FileTool::ReadState> last_read;
  auto read_it = read_state_.find(path);
  if (read_it != read_state_.end()) {
    last_read = read_it->second;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&FileTool::ReadFileBlocking, std::move(path), input.offset,
                     input.limit, input.pages, input.context_window,
                     std::move(last_read)),
      base::BindOnce(&FileTool::OnReadDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done)));
}

void FileTool::RunWriteOnUI(::nexus::generated::WriteInput input,
                            ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    std::move(done).Reject("invalid_args",
                           "file_path must be an absolute path");
    return;
  }

  // CC parity (2-3-4 item 3): skip filesystem operations for UNC paths to
  // prevent NTLM credential leaks on Windows. base::PathExists /
  // base::ReadFileToString on UNC triggers SMB authentication.
  if (base::StartsWith(input.file_path, "\\\\",
                       base::CompareCase::SENSITIVE) ||
      base::StartsWith(input.file_path, "//", base::CompareCase::SENSITIVE)) {
    std::move(done).Reject("invalid_args", "UNC paths are not supported.");
    return;
  }

  std::optional<FileTool::ReadState> last_read;
  auto read_it = read_state_.find(path);
  if (read_it != read_state_.end()) {
    last_read = read_it->second;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&FileTool::WriteFileBlocking, std::move(path),
                     std::move(input.content), std::move(last_read)),
      base::BindOnce(&FileTool::OnWriteDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done)));
}

void FileTool::RunEditOnUI(::nexus::generated::EditInput input,
                           ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  // Edit's pre-flight has more invariants than read/write.
  if (!path.IsAbsolute()) {
    std::move(done).Reject("invalid_args",
                           "file_path must be an absolute path");
    return;
  }
  if (input.old_string == input.new_string) {
    std::move(done).Reject("invalid_args",
                           "No changes to make: old_string and new_string are exactly the same.");
    return;
  }
  // CC parity (spec 2-2-4 code 5): .ipynb files must use NotebookEdit.
  if (LowercaseExtension(path) == "ipynb") {
    std::move(done).Reject(
        "notebook_file",
        "File is a Jupyter Notebook. Use the NotebookEdit tool to edit this file.");
    return;
  }

  bool replace_all = input.replace_all.value_or(false);
  std::optional<FileTool::ReadState> last_read;
  auto read_it = read_state_.find(path);
  if (read_it != read_state_.end()) {
    last_read = read_it->second;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&FileTool::EditFileBlocking, std::move(path),
                     std::move(input.old_string), std::move(input.new_string),
                     replace_all, std::move(last_read)),
      base::BindOnce(&FileTool::OnEditDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done)));
}

void FileTool::RunGlobOnUI(::nexus::generated::GlobInput input,
                           ToolCallback done) {
  if (input.pattern.empty()) {
    std::move(done).Reject("invalid_args", "pattern is required");
    return;
  }

  // Reading the session cwd is safe here because we're on UI — the
  // ToolSession owning us is alive.
  ToolSession* session = GetSession();
  base::FilePath default_root = session ? session->cwd() : base::FilePath();

  // Resolve the search root + the ripgrep `--glob` pattern. Mirrors CC's
  // GlobTool (utils/glob.ts): an ABSOLUTE `pattern` is split into its static
  // base directory (the search root) + a relative tail (the `--glob`
  // pattern), since rg's `--glob` accepts relative patterns only. A relative
  // pattern searches under `path` (or the session cwd) verbatim, letting rg
  // do the matching.
  std::string search_pattern = input.pattern;
  base::FilePath target;
  if (base::FilePath::FromUTF8Unsafe(input.pattern).IsAbsolute()) {
    auto [base_dir, tail] = ExtractGlobBaseDirectory(input.pattern);
    if (!base_dir.empty()) {
      target = base::FilePath::FromUTF8Unsafe(base_dir);
      search_pattern = std::move(tail);
    } else {
      target = ResolveAgainstCwd(input.path.value_or(""), default_root);
    }
  } else {
    target = ResolveAgainstCwd(input.path.value_or(""), default_root);
  }
  if (target.empty()) {
    std::move(done).Reject(
        "invalid_args",
        "session cwd is not set and no absolute `path` was provided. "
        "Push a cwd via setAgentLoopSettings or pass an absolute path.");
    return;
  }

  int limit = input.limit.value_or(kDefaultGlobLimit);
  if (limit <= 0) {
    limit = kDefaultGlobLimit;
  }

  // CC GlobTool parity (utils/glob.ts): `rg --files --glob <pattern>
  // --sort=modified --no-ignore --hidden <target>`. `--no-ignore` + `--hidden`
  // match CC's defaults (find gitignored / hidden files too); `--sort=modified`
  // gives mtime ordering. ripgrep replaces the old hand-rolled FileEnumerator
  // walk that overran the action budget on large checkouts. The runner owns
  // the exec + BINARY_NOT_FOUND → auto-install rg → retry flow (shared with
  // ProcessTool::Grep); OnGlobRgDone only shapes the parsed lines.
  std::vector<std::string> rg_args = {
      "--files",     "--glob",   search_pattern,       "--sort=modified",
      "--no-ignore", "--hidden", target.AsUTF8Unsafe()};

  RipgrepRunOptions options;
  // rg: cwd doesn't drive matching (the target is the trailing argv) but the
  // shell service requires an absolute, existing dir.
  options.cwd = default_root.empty() ? target : default_root;
  options.timeout_ms = static_cast<uint32_t>(kGlobTimeoutMs);
  options.max_output_bytes = static_cast<uint32_t>(kGlobMaxOutputBytes);

  ripgrep_runner_.Run(
      launcher_.get(), GetProfile(), std::move(rg_args), std::move(options),
      base::BindOnce(&FileTool::OnGlobRgDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done), limit));
}

// =============================================================================
// Worker-thread implementations — `static`, no `this`.
// =============================================================================

// static
FileTool::ReadResult FileTool::ReadFileBlocking(
    base::FilePath path,
    std::optional<int> offset,
    std::optional<int> limit,
    std::optional<std::string> pages,
    std::optional<int> context_window,
    std::optional<FileTool::ReadState> last_read) {
  ReadResult result;
  // CC parity (1-1-3 / 2-1-4): the token budget is a FIXED 25000 and does not
  // scale with the model context window, so `context_window` is no longer
  // consulted here (kept in the signature for the generated dispatcher).
  (void)context_window;
  const ReadBudget read_budget = GetReadBudget();

  base::File::Info file_info;
  base::Time last_modified;
  const bool have_file_info = base::GetFileInfo(path, &file_info);
  if (have_file_info) {
    last_modified = file_info.last_modified;
  }

  const std::string ext = LowercaseExtension(path);
  if (ext == "ipynb") {
    std::string contents;
    if (!base::ReadFileToString(path, &contents)) {
      result.error_code = "read_failed";
      result.error_message =
          base::StrCat({"failed to read file: ", path.AsUTF8Unsafe()});
      return result;
    }
    result = ReadNotebookFile(path, contents, last_modified);
    result.offset = offset;
    result.limit = limit;
    return result;
  }

  std::optional<std::string> image_mime = ImageMimeFromPath(path);
  if (image_mime.has_value()) {
    std::optional<std::vector<uint8_t>> bytes = ReadBytesWithCap(path, &result);
    if (!bytes.has_value()) {
      return result;
    }

    std::optional<std::string> sniffed = SniffImageMime(*bytes);
    result = ReadImageFile(path, *bytes, sniffed.value_or(*image_mime),
                           last_modified);
    return result;
  }

  // `pages` is a PDF-only hint, but models sometimes carry it over to ordinary
  // source-file reads. Text reads should remain useful in that case.
  pages.reset();

  // CC parity (readFileInRange throws EISDIR): a directory is not a readable
  // file. Report it distinctly so the model switches to a listing tool instead
  // of retrying, rather than collapsing to the generic "failed to read file".
  if (have_file_info && file_info.is_directory) {
    result.error_code = "is_directory";
    result.error_message = base::StrCat(
        {"EISDIR: illegal operation on a directory, read '",
         path.AsUTF8Unsafe(), "'"});
    return result;
  }

  if (!limit.has_value() && have_file_info && !file_info.is_directory &&
      file_info.size > kDefaultReadMaxSizeBytes) {
    result.error_code = "file_too_large";
    result.error_message = BuildFileTooLargeMessage(file_info.size);
    return result;
  }

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    // CC parity (FileReadTool ENOENT branch): a missing file gets a distinct
    // "File does not exist." signal so the model corrects the path instead of
    // treating it as an opaque I/O failure.
    if (!base::PathExists(path)) {
      result.error_code = "file_not_found";
      result.error_message = "File does not exist.";
      return result;
    }
    result.error_code = "read_failed";
    result.error_message =
        base::StrCat({"failed to read file: ", path.AsUTF8Unsafe()});
    return result;
  }

  // CC parity (utils/fileRead.ts:20-49): detect BOM-based encoding and transcode
  // UTF-16LE → UTF-8 so the model sees readable text (and so the binary
  // heuristic below doesn't trip on UTF-16LE's null bytes). `contents` keeps
  // the RAW on-disk bytes for the read_state_ cache + dedup/staleness symmetry;
  // `text` is the UTF-8 view used for projection + token estimation.
  bool is_utf16le = false;
  const std::string text = DecodeToUTF8(contents, &is_utf16le);

  if (!is_utf16le && net::LooksLikeBinary(contents)) {
    result.error_code = "unsupported_binary_file";
    result.error_message =
        "File appears to be binary and is not a supported Read media type. "
        "Use a dedicated binary-capable tool or convert it to text first.";
    return result;
  }

  if (last_read.has_value() && last_read->from_read &&
      last_read->offset == offset && last_read->limit == limit &&
      !pages.has_value() && !last_modified.is_null() &&
      last_modified == last_read->last_modified &&
      contents == last_read->content) {
    result.ok = true;
    result.message = kFileUnchangedStub;
    return result;
  }

  int start_line = offset.value_or(1);
  if (start_line < 1) {
    start_line = 1;
  }
  // CC parity (2-1-9 / 2-1-10): `MAX_LINES_TO_READ=2000` is prompt-only and NOT
  // enforced. The only line cap is the caller's explicit `limit`; with no limit
  // the whole file from `start` is projected (overflow is caught by the
  // post-read token guard, which throws rather than truncating).
  std::optional<int> max_lines;
  if (limit.has_value() && *limit >= 1) {
    max_lines = *limit;
  }

  // CC parity (readFileInRange strips a leading U+FEFF before line-splitting):
  // present the model BOM-free content so an Edit `old_string` copied from line
  // 1 doesn't silently capture the BOM. The strip is applied ONLY to the
  // model-facing projection + its token-estimate `content`; the cached
  // `full_content` and the dedup/staleness comparison keep the raw on-disk
  // bytes (the Write/Edit re-read is also raw), so that comparison stays
  // symmetric. For UTF-16LE, DecodeToUTF8 already consumed the BOM, so `text`
  // is BOM-free; the UTF-8 BOM strip below handles the UTF-8 case.
  std::string_view projection_source = text;
  if (projection_source.size() >= 3 &&
      projection_source.substr(0, 3) == "\xEF\xBB\xBF") {
    projection_source = projection_source.substr(3);
  }

  ProjectionResult projection =
      BuildLineNumberedProjection(projection_source, start_line, max_lines);

  // CC parity (FileReadTool.mapToolResultToToolResultBlockParam, `text`
  // case): when the selected range yields no content the model must
  // receive a system-reminder warning *in place of* file content — never
  // an empty result. Without this, an empty file (or an offset past EOF)
  // produced an empty `message`/`payload`, and OnReadDone resolved with an
  // empty dict: the model saw `{}` instead of the documented warning, and
  // the read state was never recorded (so a follow-up Write to a freshly
  // read empty file was wrongly rejected as "not read yet"). Mirror CC's
  // two warning strings, distinguishing a genuinely empty file from an
  // offset past the end of file.
  if (projection.lines_emitted == 0) {
    result.ok = true;
    result.file_path = std::move(path);
    result.full_content = contents;
    result.last_modified = last_modified;
    result.offset = offset;
    result.limit = limit;
    // CC parity (2-1-8 / 2-1-10 inv.4): Read NEVER sets isPartialView — a
    // partial read does not block a later Edit/Write (the staleness guard is
    // the content comparison, not the read's range).
    result.cache_read_state = true;
    if (projection.total_lines == 0) {
      result.message =
          "<system-reminder>Warning: the file exists but the contents are "
          "empty.</system-reminder>";
    } else {
      result.message = base::StrCat(
          {"<system-reminder>Warning: the file exists but is shorter than the "
           "provided offset (",
           base::NumberToString(start_line), "). The file has ",
           base::NumberToString(projection.total_lines),
           " lines.</system-reminder>"});
    }
    return result;
  }

  const int token_estimate = EstimateReadTokens(projection.content, ext);
  if (token_estimate > read_budget.max_output_tokens) {
    result.error_code = "max_file_read_token_exceeded";
    result.error_message =
        BuildTokenLimitMessage(token_estimate, read_budget.max_output_tokens);
    return result;
  }

  // CC parity: Read returns the line-numbered content directly (CC's
  // FileReadTool emits "cat -n" formatted text as the tool_result string).
  // No truncation footer is appended — CC never silently truncates; overflow
  // is enforced by the token guard above, which throws (2-1-4.5).
  result.ok = true;
  result.file_path = std::move(path);
  result.full_content = contents;
  result.last_modified = last_modified;
  result.offset = offset;
  result.limit = limit;
  // CC parity (2-1-8 / 2-1-10 inv.4): Read never sets isPartialView.
  result.cache_read_state = true;
  result.message = std::move(projection.projection);
  return result;
}

// static
FileTool::WriteResult FileTool::WriteFileBlocking(
    base::FilePath path,
    std::string content,
    std::optional<FileTool::ReadState> last_read) {
  WriteResult result;

  // Probe existence BEFORE we touch the file so the prose response can
  // distinguish create-vs-update (CC's FileWriteTool branches on the
  // same signal). base::WriteFile overwrites on create, so without a
  // prior probe we couldn't tell which case applies.
  const bool was_create = !base::PathExists(path);

  // CC parity: overwriting an existing file requires a previous full Read and a
  // matching on-disk content snapshot. New files do not require a prior Read.
  // The single read below serves BOTH the staleness guard and the UI-diff
  // snapshot — reading twice would be a TOCTOU window where a concurrent
  // writer slips content past the guard.
  std::string original_contents;
  bool have_original = false;
  if (!was_create) {
    if (!last_read.has_value() || last_read->is_partial_view) {
      result.error_code = "file_not_read";
      result.error_message =
          "File must be fully read before editing. A partial read (using offset/limit) does not count, read the entire file first, then retry the edit.";
      return result;
    }
    have_original = base::ReadFileToString(path, &original_contents);
    const bool content_unchanged =
        have_original && original_contents == last_read->content;
    if (!content_unchanged) {
      result.error_code = "file_modified_since_read";
      result.error_message =
          "File has been modified since read, either by the user or by a "
          "linter. Read it again before attempting to write it.";
      return result;
    }
  }

  // Ensure the parent directory exists. base::WriteFile fails silently
  // (returns false) when the parent is missing; creating it explicitly
  // mirrors Claude Code's behavior of working from a clean slate when
  // the agent writes a file under a not-yet-existing directory tree.
  base::FilePath parent = path.DirName();
  if (!parent.empty() && !base::DirectoryExists(parent)) {
    if (!base::CreateDirectory(parent)) {
      result.error_code = "write_failed";
      result.error_message = base::StrCat(
          {"failed to create parent directory: ", parent.AsUTF8Unsafe()});
      return result;
    }
  }

  // CC parity (2-3-7): Write ALWAYS outputs LF. The model sends explicit
  // line endings; do not preserve the old file's CRLF (contrast Edit which
  // restores original line endings via RestoreCRLF).
  content = NormalizeCRLF(content);

  // CC parity (2-3-7.2): preserve the original file's BOM-based encoding.
  // The write bytes may differ from |content| (e.g. UTF-16LE transcoding),
  // but ReadState stores the raw on-disk bytes so the staleness check
  // (raw-vs-raw) stays symmetric with Read.
  std::string write_bytes = content;
  if (have_original) {
    write_bytes = PreserveOriginalEncoding(original_contents, content);
  }

  if (!base::WriteFile(path, write_bytes)) {
    result.error_code = "write_failed";
    result.error_message =
        base::StrCat({"failed to write file: ", path.AsUTF8Unsafe()});
    return result;
  }

  // Result-string design (latest iteration — see D7 entry in
  // C:\Users\myyyb\.claude\plans\compare-the-cc-and-staged-reddy.md and
  // docs/agents/nexus-web-parity-audit.md → "Write tool success message"):
  //
  // CC's wording is reused verbatim ("File created successfully at: <p>"
  // / "The file <p> has been updated successfully.") so the model's
  // trained recognition pattern fires. CC's wording on its own was not
  // enough in Nexus — testing showed the model still re-issued Write
  // calls because the result didn't confirm the content actually
  // landed on disk (it only confirmed the file existed afterwards).
  // The fix is the additive ". Wrote N bytes." suffix: it carries an
  // unambiguous byte-count signal that the agent's content was
  // accepted, which closes the verify-loop the model was caught in.
  //
  // Echoing the path is intentional — earlier hypotheses worried it
  // would "create ambiguity about whether the SAME path was written",
  // but the opposite proved true: the path echo lets the model
  // pattern-match what it sent vs. what landed in one comparison,
  // and is consistent with CC's wording.
  //
  // Earlier wording variants in this slot:
  //  - "The function is executed successfully; the file is created and
  //    the content is updated too." (generic, ambiguous about content)
  //  - "The function is executed successfully; the content is updated."
  //    (update-only variant; same ambiguity)
  // Both produced retry spirals. Do not revert to either.
  result.ok = true;
  const std::string byte_str = base::NumberToString(write_bytes.size());
  result.message =
      was_create
          ? base::StrCat({"File created successfully at: ", path.AsUTF8Unsafe(),
                          ". Wrote ", byte_str, " bytes."})
          : base::StrCat({"The file ", path.AsUTF8Unsafe(),
                          " has been updated successfully. Wrote ", byte_str,
                          " bytes."});
  if (have_original) {
    result.original_file = std::move(original_contents);
    result.original_file_set = true;
  }
  result.file_path = std::move(path);
  result.updated_content = std::move(write_bytes);
  base::File::Info after_info;
  if (base::GetFileInfo(result.file_path, &after_info)) {
    result.last_modified = after_info.last_modified;
  }
  return result;
}

// static
FileTool::EditResult FileTool::EditFileBlocking(
    base::FilePath path,
    std::string old_string,
    std::string new_string,
    bool replace_all,
    std::optional<FileTool::ReadState> last_read) {
  EditResult result;

  // CC parity (spec 2-2-4 code 10): reject files > 1 GiB to prevent OOM.
  base::File::Info pre_info;
  if (base::GetFileInfo(path, &pre_info) &&
      pre_info.size > kMaxEditFileSize) {
    result.error_code = "file_too_large";
    result.error_message = base::StrCat(
        {"File is too large to edit (", FormatFileSize(pre_info.size),
         "). Maximum editable file size is ",
         FormatFileSize(kMaxEditFileSize), "."});
    return result;
  }

  // CC parity (spec 2-2-4 codes 4/7): file creation path — old_string empty
  // and file doesn't exist means new file creation. No prior read required.
  const bool file_exists = base::PathExists(path);
  if (!file_exists) {
    if (old_string.empty()) {
      // File creation: write new_string as the file content.
      base::FilePath parent = path.DirName();
      if (!parent.empty() && !base::DirectoryExists(parent)) {
        base::CreateDirectory(parent);
      }
      if (!base::WriteFile(path, new_string)) {
        result.error_code = "write_failed";
        result.error_message = base::StrCat(
            {"failed to write file: ", path.AsUTF8Unsafe()});
        return result;
      }
      result.ok = true;
      result.message = base::StrCat({"The file ", path.AsUTF8Unsafe(),
                                     " has been updated successfully."});
      result.file_path = std::move(path);
      result.updated_content = std::move(new_string);
      base::File::Info after_info;
      if (base::GetFileInfo(result.file_path, &after_info)) {
        result.last_modified = after_info.last_modified;
      }
      return result;
    }
    // File doesn't exist and old_string is non-empty.
    result.error_code = "file_not_found";
    result.error_message = base::StrCat(
        {"File does not exist. Current working directory: ",
         path.DirName().AsUTF8Unsafe(), "."});
    return result;
  }

  // CC parity (spec 2-2-4 code 6): file must have been fully read first.
  if (!last_read.has_value()) {
    result.error_code = "file_not_read";
    result.error_message =
        "File has not been read yet. Read it first before writing to it.";
    return result;
  }
  if (last_read->is_partial_view) {
    const std::string offset_str =
        last_read->offset.has_value()
            ? base::NumberToString(*last_read->offset)
            : "start";
    const std::string limit_str =
        last_read->limit.has_value()
            ? base::NumberToString(*last_read->limit)
            : "end";
    result.error_code = "file_not_read";
    result.error_message = base::StrCat(
        {"File was only partially read (offset: ", offset_str,
         ", limit: ", limit_str, "). "
         "Read the full file without offset/limit before editing."});
    return result;
  }

  // CC parity (spec 2-2-4 code 3): empty old_string on existing non-empty
  // file is a create-over-existing error.
  if (old_string.empty()) {
    std::string existing;
    base::ReadFileToString(path, &existing);
    if (!existing.empty() &&
        base::TrimWhitespaceASCII(existing, base::TRIM_ALL).size() > 0) {
      result.error_code = "file_already_exists";
      result.error_message = "Cannot create new file - file already exists.";
      return result;
    }
    // Empty file with empty old_string: replace empty with new_string.
    if (!base::WriteFile(path, new_string)) {
      result.error_code = "write_failed";
      result.error_message = base::StrCat(
          {"failed to write file: ", path.AsUTF8Unsafe()});
      return result;
    }
    result.ok = true;
    result.message = base::StrCat({"The file ", path.AsUTF8Unsafe(),
                                   " has been updated successfully."});
    result.original_file = std::move(existing);
    result.original_file_set = true;
    result.file_path = std::move(path);
    result.updated_content = std::move(new_string);
    base::File::Info after_info;
    if (base::GetFileInfo(result.file_path, &after_info)) {
      result.last_modified = after_info.last_modified;
    }
    return result;
  }

  // Single read serves both the staleness guard and the edit input — a
  // second read would open a TOCTOU window where the edit applies to content
  // the guard never validated.
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    result.error_code = "read_failed";
    result.error_message =
        base::StrCat({"failed to read file for edit: ", path.AsUTF8Unsafe()});
    return result;
  }

  // CC parity (spec 2-2-4 code 7): staleness check. Compare raw bytes first
  // (symmetric with read_state_ which stores raw). If raw differs, the file
  // was modified since read.
  if (contents != last_read->content) {
    result.error_code = "file_modified_since_read";
    result.error_message =
        "File has been modified since read, either by the user or by a "
        "linter. Read it again before attempting to write it.";
    return result;
  }

  // CC parity (utils/fileRead.ts:20-49): decode BOM-based encoding to UTF-8
  // for matching. The model's old_string/new_string are UTF-8; a UTF-16LE file
  // must be transcoded before substring matching can work. `contents` keeps the
  // RAW bytes for the read_state_ cache + re-encode on write; `work_content`
  // starts as the UTF-8 view.
  bool is_utf16le = false;
  std::string work_content = DecodeToUTF8(contents, &is_utf16le);

  // CC parity (spec 2-2-8): normalize CRLF → LF for matching. The model sees
  // LF-normalized content from Read (the projection strips \r), so old_string
  // will be LF. The file on disk may be CRLF.
  const bool is_crlf = HasCRLF(work_content);
  if (is_crlf) {
    work_content = NormalizeCRLF(work_content);
  }

  // CC parity (utils.ts normalizeFileEditInput): strip trailing whitespace
  // from new_string for non-markdown files.
  const std::string ext = LowercaseExtension(path);
  const bool is_markdown = (ext == "md" || ext == "mdx");
  std::string work_new_string =
      is_markdown ? new_string : StripTrailingWhitespace(new_string);

  // CC parity (utils.ts findActualString): exact match first, then
  // quote-normalized fallback.
  bool found = false;
  std::string actual_old_string =
      FindActualString(work_content, old_string, &found);

  // CC parity (utils.ts normalizeFileEditInput / DESANITIZATIONS): if the
  // exact/quote-normalized match fails, try desanitizing old_string (the model
  // emits sanitized tokens it can't see) and mirror the same replacements onto
  // new_string.
  std::vector<std::pair<std::string, std::string>> applied_desanitizations;
  if (!found) {
    const std::string desanitized_old =
        DesanitizeString(old_string, &applied_desanitizations);
    if (!applied_desanitizations.empty()) {
      actual_old_string =
          FindActualString(work_content, desanitized_old, &found);
      if (found) {
        for (const auto& [from, to] : applied_desanitizations) {
          base::ReplaceSubstringsAfterOffset(&work_new_string, 0, from, to);
        }
      }
    }
  }

  if (!found) {
    result.error_code = "no_match";
    result.error_message = base::StrCat(
        {"String to replace not found in file.\nString: ", old_string});
    return result;
  }

  // CC parity (utils.ts preserveQuoteStyle): when the file uses curly quotes
  // and the model sent straight quotes, apply curly style to new_string.
  work_new_string =
      PreserveQuoteStyle(old_string, actual_old_string, work_new_string);

  // CC parity (spec 2-2-4 code 9): uniqueness check.
  const size_t occurrences =
      CountOccurrences(work_content, actual_old_string);
  if (occurrences > 1 && !replace_all) {
    result.error_code = "ambiguous_match";
    result.error_message = base::StrCat(
        {"Found ", base::NumberToString(occurrences),
         " matches of the string to replace, but replace_all is false. "
         "To replace all occurrences, set replace_all to true. To replace "
         "only one occurrence, please provide more context to uniquely "
         "identify the instance.\nString: ",
         old_string});
    return result;
  }

  // CC parity (utils.ts applyEditToFile): apply with delete-strips-newline.
  // `updated` is the UTF-8, LF-normalized post-edit content. `work_content`
  // still holds the decoded + LF-normalized PRE-edit content (what old_string
  // matched against).
  std::string updated = ApplyEditToFile(work_content, actual_old_string,
                                        work_new_string, replace_all);
  if (updated == work_content) {
    result.error_code = "no_match";
    result.error_message =
        "String not found in file. Failed to apply edit.";
    return result;
  }

  // CC parity (spec 2-2-8): restore original line endings, then re-encode to the
  // original BOM-based encoding before writing. `write_bytes` is what lands on
  // disk — for UTF-16LE this re-introduces the BOM + little-endian code units.
  if (is_crlf) {
    updated = RestoreCRLF(updated);
  }
  const std::string write_bytes =
      is_utf16le ? PreserveOriginalEncoding(contents, updated) : updated;

  if (!base::WriteFile(path, write_bytes)) {
    result.error_code = "write_failed";
    result.error_message = base::StrCat(
        {"failed to write back after edit: ", path.AsUTF8Unsafe()});
    return result;
  }

  // CC parity: prose success string. Match FileEditTool.ts wording exactly.
  result.ok = true;
  result.message =
      replace_all ? base::StrCat({"The file ", path.AsUTF8Unsafe(),
                                  " has been updated. All occurrences were "
                                  "successfully replaced."})
                  : base::StrCat({"The file ", path.AsUTF8Unsafe(),
                                  " has been updated successfully."});
  // Carry the pre-edit content on a UI-only side channel (see OnEditDone) so the
  // browser can render a file-relative diff. It never reaches the model. This is
  // the decoded + LF-normalized view (what old_string matched), so the JS diff
  // works for UTF-16LE and CRLF alike — CC likewise diffs the LF-normalized
  // content (readFileForEdit normalizes on read).
  result.original_file = std::move(work_content);
  result.original_file_set = true;
  result.file_path = std::move(path);
  // read_state_ caches raw on-disk bytes (symmetric with Read), so store the
  // re-encoded bytes actually written, not the UTF-8 `updated`.
  result.updated_content = std::move(write_bytes);
  base::File::Info after_info;
  if (base::GetFileInfo(result.file_path, &after_info)) {
    result.last_modified = after_info.last_modified;
  }
  return result;
}

// static
std::pair<std::string, std::string> FileTool::ExtractGlobBaseDirectory(
    const std::string& pattern_in) {
  // Faithful port of claude_code utils/glob.ts `extractGlobBaseDirectory`.
  // Splits `pattern` into (baseDir, relativePattern) at the first glob
  // metacharacter. RunGlobOnUI applies it only to ABSOLUTE patterns, to feed
  // rg a concrete search root + a relative `--glob` tail.
  std::string pattern = pattern_in;
  std::replace(pattern.begin(), pattern.end(), '\\', '/');

  // Windows drive root: "C:" is drive-relative; the real root is "C:/".
  auto fix_drive_root = [](std::string dir) -> std::string {
    if (dir.size() == 2 && base::IsAsciiAlpha(dir[0]) && dir[1] == ':') {
      dir.push_back('/');
    }
    return dir;
  };

  size_t glob_pos = pattern.find_first_of("*?[{");
  if (glob_pos == std::string::npos) {
    // No glob chars — a literal path: baseDir = dirname, tail = basename.
    size_t last_sep = pattern.find_last_of('/');
    if (last_sep == std::string::npos) {
      return {std::string(), pattern};
    }
    std::string base_dir =
        last_sep == 0 ? std::string("/") : pattern.substr(0, last_sep);
    return {fix_drive_root(std::move(base_dir)), pattern.substr(last_sep + 1)};
  }

  // The static prefix is everything before the first glob char; narrow at its
  // last separator.
  size_t last_sep = pattern.rfind('/', glob_pos);
  if (last_sep == std::string::npos) {
    // No separator before the glob — pattern is relative to the search root.
    return {std::string(), pattern};
  }
  // Leading-slash root (e.g. "/*.txt"): baseDir collapses to "/".
  std::string base_dir =
      last_sep == 0 ? std::string("/") : pattern.substr(0, last_sep);
  return {fix_drive_root(std::move(base_dir)), pattern.substr(last_sep + 1)};
}

// =============================================================================
// UI-thread reply trampolines.
// =============================================================================

void FileTool::OnReadDone(ToolCallback done, ReadResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                           std::move(result.error_message));
    return;
  }
  if (!result.message.empty()) {
    if (!result.file_path.empty() && result.cache_read_state) {
      ReadState state;
      state.content = result.full_content;
      state.last_modified = result.last_modified;
      state.is_partial_view = result.is_partial_view;
      state.offset = result.offset;
      state.limit = result.limit;
      state.from_read = true;
      read_state_[result.file_path] = std::move(state);
    }
    std::move(done).Resolve(base::Value(std::move(result.message)));
    return;
  }
  if (!result.payload.empty()) {
    if (!result.file_path.empty() && result.cache_read_state) {
      ReadState state;
      state.content = result.full_content;
      state.last_modified = result.last_modified;
      state.is_partial_view = result.is_partial_view;
      state.offset = result.offset;
      state.limit = result.limit;
      state.from_read = true;
      read_state_[result.file_path] = std::move(state);
    }
    std::move(done).Resolve(base::Value(std::move(result.payload)));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnReadMediaDone(
    base::FilePath path,
    ToolCallback done,
    bool ok,
    ::nexus::mojom::MediaReadResultPtr result,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message) {
  if (!ok || !result) {
    std::move(done).Reject(error_code.value_or("media_read_failed"),
                           error_message.value_or("failed to read media"));
    return;
  }

  base::ListValue parts;
  for (auto& media_part : result->parts) {
    if (!media_part) {
      continue;
    }
    if (media_part->type == "text") {
      AppendTextPart(&parts, media_part->text.value_or(std::string()));
      continue;
    }
    if (!media_part->data.has_value()) {
      continue;
    }
    const std::string mime = media_part->mime_type.value_or(
        media_part->type == "document" ? std::string("application/pdf")
                                       : std::string("image/jpeg"));
    const std::string data_base64 =
        base::Base64Encode(base::span(*media_part->data));
    if (media_part->type == "document") {
      AppendDocumentPart(&parts, data_base64, mime);
    } else if (media_part->type == "image") {
      AppendImagePart(&parts, data_base64, mime);
    }
  }

  // CC parity (FileReadTool.callInner records readFileState for notebooks):
  // give notebook reads a native read-state entry so a subsequent Edit/Write
  // on the .ipynb passes the "file has been read" precondition. Only notebooks
  // populate `full_text_for_cache`; images/PDFs leave it null and — exactly
  // like CC — record no read-state (they aren't text-editable). `last_modified`
  // is left null: Edit/Write gate on a content comparison (not mtime), and the
  // media read path has no Read-dedup that would consult mtime.
  if (result->full_text_for_cache.has_value()) {
    ReadState state;
    state.content = *result->full_text_for_cache;
    state.is_partial_view = false;
    state.from_read = true;
    read_state_[path] = std::move(state);
  }

  base::DictValue payload;
  payload.Set("message",
              base::StrCat({"Media file read: ", path.AsUTF8Unsafe()}));
  payload.Set("content_parts", std::move(parts));
  std::move(done).Resolve(base::Value(std::move(payload)));
}

void FileTool::OnWriteDone(ToolCallback done, WriteResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                           std::move(result.error_message));
    return;
  }
  // On success, forward a structured dict `{ message, original_file? }`. The JS
  // hostedTools.ts pulls `message` back out as the model-facing prose (so the
  // model still sees the CC-style string, never the JSON) and uses
  // `original_file` — present only when overwriting — to render a file-relative
  // diff. Absent original_file → the JS side treats it as a newly created file.
  if (!result.file_path.empty()) {
    ReadState state;
    state.content = result.updated_content;
    state.last_modified = result.last_modified;
    state.is_partial_view = false;
    read_state_[result.file_path] = std::move(state);
  }
  if (!result.message.empty()) {
    base::DictValue dict;
    dict.Set("message", std::move(result.message));
    if (result.original_file_set) {
      // original_file may be raw UTF-16LE bytes (not valid UTF-8) — decode
      // for the JS diff renderer which expects a UTF-8 string.
      bool ignored = false;
      dict.Set("original_file",
               DecodeToUTF8(result.original_file, &ignored));
    }
    std::move(done).Resolve(std::move(dict));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnEditDone(ToolCallback done, EditResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                           std::move(result.error_message));
    return;
  }
  // On success, forward a structured dict `{ message, original_file }`. The JS
  // hostedTools.ts pulls `message` back out as the model-facing prose (so the
  // model still sees the CC-style string, never the JSON) and uses
  // `original_file` to render a file-relative diff. See OnWriteDone.
  if (!result.file_path.empty()) {
    ReadState state;
    state.content = result.updated_content;
    state.last_modified = result.last_modified;
    state.is_partial_view = false;
    read_state_[result.file_path] = std::move(state);
  }
  if (!result.message.empty()) {
    base::DictValue dict;
    dict.Set("message", std::move(result.message));
    if (result.original_file_set) {
      // original_file may be raw UTF-16LE bytes (not valid UTF-8) — decode
      // for the JS diff renderer which expects a UTF-8 string.
      bool ignored = false;
      dict.Set("original_file",
               DecodeToUTF8(result.original_file, &ignored));
    }
    std::move(done).Resolve(std::move(dict));
    return;
  }
  std::move(done).Resolve(std::move(result.payload));
}

void FileTool::OnGlobRgDone(ToolCallback done,
                            int limit,
                            RipgrepRunResult result) {
  // Pre-launch failure: rg was unavailable and the runner's one-shot
  // auto-install didn't recover it. The runner already shaped a diagnostic
  // message (+ manual-install hint); surface it under a Glob-labeled code.
  if (!result.launched) {
    std::string code = result.error_code.empty() ? std::string("glob_failed")
                                                 : std::move(result.error_code);
    std::move(done).Reject(std::move(code), std::move(result.error_message));
    return;
  }

  std::vector<std::string>& lines = result.lines;

  // CC parity (GlobTool.mapToolResultToToolResultBlockParam): "No files found"
  // on empty, otherwise the matched paths joined by '\n' with a truncation
  // caveat when there were more than `limit`. The model uses an anchored Glob
  // (the exact path as the pattern) as a file-exists probe and keys off the
  // "No files found" sentinel — rg exits 1 (no matches) → empty lines here.
  if (lines.empty()) {
    std::move(done).Resolve(base::Value("No files found"));
    return;
  }
  bool truncated = static_cast<int>(lines.size()) > limit;
  if (truncated) {
    lines.resize(limit);
  }
  std::string body = base::JoinString(lines, "\n");
  if (truncated) {
    body.append(
        "\n(Results are truncated. Consider using a more specific pattern "
        "or increasing the limit.)");
  }
  std::move(done).Resolve(base::Value(std::move(body)));
}

}  // namespace nexus

// Definition matching the NEXUS_DECLARE_CATEGORY in the header. Lives
// in the .cc so it can include `tool_session.h` (the header can't —
// `tool_session.h` itself includes file_tool.h to embed the FileTool
// member). The dispatcher in nexus_tools_registry.gen.cc invokes this
// via `CategoryAccessor<FileTool>::Get(session)`.
NEXUS_DEFINE_CATEGORY(nexus::FileTool, file_tool);
