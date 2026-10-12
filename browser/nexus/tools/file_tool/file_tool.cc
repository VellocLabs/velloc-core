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

// A Read without `limit` is refused before any I/O when the whole file is
// larger than 0.25 MiB (spec 2-1-4: the gate is on TOTAL file size). An
// explicit `limit` skips this guard so targeted reads of large files still
// work.
constexpr int64_t kDefaultReadMaxSizeBytes = 256 * 1024;

// Read max output budget (spec 1-1-3 / 2-1-4): a FIXED 25000-token cap,
// independent of the active model's context window. Nexus cannot call the model
// token-count API from native C++, so it approximates tokens by file type
// (EstimateReadTokens below). The prior context-window-scaled 12500/25000
// budget was removed to match the spec.
constexpr int kReadMaxOutputTokens = 25000;

constexpr size_t kMediaReadByteCap = 12 * 1024 * 1024;  // raw bytes

// The most PDF pages a single Read may request. A range larger than this is rejected with
// errorCode 8 (spec 2-1-5).
constexpr int kPdfMaxPagesPerRead = 20;

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

struct ReadBudget {
  int max_output_tokens;
};

ReadBudget GetReadBudget() {
  return {kReadMaxOutputTokens};
}

std::string LowercaseExtension(const base::FilePath& path) {
  std::string ext = base::FilePath(path.Extension()).AsUTF8Unsafe();
  if (!ext.empty() && ext.front() == '.') {
    ext.erase(ext.begin());
  }
  return base::ToLowerASCII(ext);
}

// Parse a PDF `pages` string (spec 2-1-5) into a (first, last) 1-indexed range. Supports "5", "1-10", and the
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

// Descriptors 0-2: stdin, stdout, stderr.
bool IsStdioDescriptor(std::string_view name) {
  return name == "0" || name == "1" || name == "2";
}

// True when the path names a POSIX device a Read could never finish
// (spec 2-1-5, errorCode 9). Path-only, no I/O. Two kinds hang a read:
// devices that produce bytes forever, and the process's own terminal / stdio
// streams, which block waiting for input. /dev/null and other finite devices
// are allowed. The Windows host never resolves these paths; the rule is part
// of the Read contract and guards a POSIX host.
bool IsBlockedDevicePath(const std::string& path) {
  constexpr std::string_view kDev = "/dev/";
  if (base::StartsWith(path, kDev, base::CompareCase::SENSITIVE)) {
    const std::string_view name = std::string_view(path).substr(kDev.size());
    // Endless output.
    if (name == "zero" || name == "full" || name == "random" ||
        name == "urandom") {
      return true;
    }
    // Interactive streams.
    if (name == "tty" || name == "console" || name == "stdin" ||
        name == "stdout" || name == "stderr") {
      return true;
    }
    constexpr std::string_view kFd = "fd/";
    return base::StartsWith(name, kFd, base::CompareCase::SENSITIVE) &&
           IsStdioDescriptor(name.substr(kFd.size()));
  }
  // Linux exposes the same stdio streams as /proc/<pid or self>/fd/<n>.
  if (base::StartsWith(path, "/proc/", base::CompareCase::SENSITIVE)) {
    const size_t slash = path.rfind('/');
    return slash != std::string::npos &&
           base::EndsWith(std::string_view(path).substr(0, slash), "/fd",
                          base::CompareCase::SENSITIVE) &&
           IsStdioDescriptor(std::string_view(path).substr(slash + 1));
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
                                            base::DictValue media,
                                            base::ListValue parts,
                                            base::Time last_modified) {
  FileTool::ReadResult result;
  result.ok = true;
  media.Set("path", path.AsUTF8Unsafe());
  result.file_path = std::move(path);
  result.last_modified = last_modified;
  result.payload.Set("media", std::move(media));
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
    result->error_params.Set("kind", "media");
    result->error_params.Set("path", path.AsUTF8Unsafe());
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(path);
  if (!bytes.has_value()) {
    result->error_code = "read_failed";
    result->error_params.Set("path", path.AsUTF8Unsafe());
    return std::nullopt;
  }
  if (bytes->size() > kMediaReadByteCap) {
    result->error_code = "file_too_large";
    result->error_params.Set("kind", "media");
    result->error_params.Set("path", path.AsUTF8Unsafe());
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
    result.error_params.Set("reason", "parse");
    return result;
  }

  const base::DictValue& notebook = parsed->GetDict();
  const base::ListValue* cells = notebook.FindList("cells");
  if (!cells) {
    result.error_code = "invalid_notebook";
    result.error_params.Set("reason", "no_cells");
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

  base::DictValue media;
  media.Set("kind", "notebook");
  media.Set("cells", static_cast<int>(cells->size()));
  result = MakeContentPartsResult(path, std::move(media), std::move(parts),
                                  last_modified);
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
  AppendImagePart(&parts, base::Base64Encode(encoded.bytes),
                  std::move(encoded.mime_type));
  base::DictValue media;
  media.Set("kind", "image");
  media.Set("bytes", static_cast<double>(bytes.size()));
  return MakeContentPartsResult(path, std::move(media), std::move(parts),
                                last_modified);
}

// Read's line-numbered projection ("1\tline\n…"), part of the Read contract
// since Edit copies from it. Starts at `start_line_number`; `max_lines` caps
// lines, with no default or byte cap (the token guard throws). `content` is
// the unprefixed text for the token check.
struct ProjectionResult {
  std::string projection;
  std::string content;
  int lines_emitted = 0;
  // Total number of lines in the file (after stripping the phantom
  // trailing entry of a "\n"-terminated file). Used by the caller to
  // emit the offset-past-EOF warning, which reports the file length.
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
  // (no default line cap, no byte cap — spec 2-1-9).
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
// Edit helpers — matching and rewriting rules from spec 2-2
// =============================================================================

// Largest file Edit accepts: 1 GiB (stat bytes). Prevents OOM on multi-GB
// files (spec 2-2-4 code 10).
constexpr int64_t kMaxEditFileSize = 1024 * 1024 * 1024;

// Detect whether content uses CRLF line endings.
bool HasCRLF(const std::string& content) {
  return content.find("\r\n") != std::string::npos;
}

// Normalize CRLF → LF before matching (spec 2-2-8). A standalone \r is left
// untouched; only \r\n pairs are folded.
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
// content to match, so a write keeps the file's encoding (spec 2-3-7.2).
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

// Decode raw on-disk bytes to UTF-8 for text projection/matching: a
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

// Fold curly quotes (U+2018/2019/201C/201D) to straight ones for
// quote-tolerant matching. `orig_offsets` maps each normalized byte to its
// original offset (plus an end sentinel), since the fold changes lengths.
std::string NormalizeQuotes(const std::string& str,
                            std::vector<size_t>* orig_offsets = nullptr) {
  std::string result;
  result.reserve(str.size());
  if (orig_offsets) {
    orig_offsets->clear();
    orig_offsets->reserve(str.size() + 1);
  }
  for (size_t i = 0; i < str.size();) {
    if (orig_offsets) {
      orig_offsets->push_back(i);
    }
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
  if (orig_offsets) {
    orig_offsets->push_back(str.size());  // end sentinel
  }
  return result;
}

// Exact match first, then quote-normalized (spec 2-2-6).
// Returns the actual substring from file_content that matches search_string.
// The quote-normalized fallback must map the match BACK through the offset
// map: a curly quote before the match shifts every later normalized position,
// and a curly quote inside the match makes the original slice longer than the
// search string — indexing file_content with normalized positions (the old
// code) replaced the wrong bytes or split a quote mid-sequence.
std::string FindActualString(const std::string& file_content,
                             const std::string& search_string,
                             bool* found) {
  if (file_content.find(search_string) != std::string::npos) {
    *found = true;
    return search_string;
  }
  const std::string normalized_search = NormalizeQuotes(search_string);
  std::vector<size_t> orig_offsets;
  const std::string normalized_file =
      NormalizeQuotes(file_content, &orig_offsets);
  const size_t pos = normalized_file.find(normalized_search);
  if (pos != std::string::npos) {
    *found = true;
    const size_t begin = orig_offsets[pos];
    const size_t end = orig_offsets[pos + normalized_search.size()];
    return file_content.substr(begin, end - begin);
  }
  *found = false;
  return std::string();
}

// Whether a quote at `byte_pos` opens a quotation: start of text, or after
// whitespace or an opening bracket.
bool IsOpeningContext(const std::string& str, size_t byte_pos) {
  if (byte_pos == 0) {
    return true;
  }
  const char prev = str[byte_pos - 1];
  return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
         prev == '(' || prev == '[' || prev == '{';
}

// When old_string matched only via quote normalization, write new_string in
// the file's curly quote style (spec 2-2-6).
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

// Strip trailing whitespace per line.
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

// Model-side token abbreviations (spec 2-2-6). The model API abbreviates a
// few reserved XML-like tokens, so a model quoting a file that contains them
// writes the short form. When an edit misses, these pairs map the short form
// back to the file's text. The pairs are a fact of the model's output, not a
// design choice; see the copyright plan, S5-10.
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

// Apply one edit (spec 2-2-6).
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

bool IsBlockedDevicePathForTesting(const std::string& path) {
  return IsBlockedDevicePath(path);
}

// =============================================================================
// Out-of-line, move-only special members for the result structs.
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
  // Invalidate WeakPtrs only on teardown reasons, so in-flight replies drop;
  // per-turn reasons have no in-flight I/O.
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
// Run<Name>OnUI overrides (runtime.thread = "ui"): validate on UI (rejecting
// inline), hop the static `<Name>Blocking` helper to base::ThreadPool
// {MayBlock, USER_VISIBLE}, and reply on UI through GetWeakPtr(), so a torn
// down session is never touched.
// =============================================================================

void FileTool::RunReadOnUI(::nexus::generated::FilesReadInput input,
                           ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    base::DictValue params;
    params.Set("reason", "file_path_not_absolute");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  // Spec 2-1-5: validate the `pages`
  // parameter BEFORE any I/O. Bad format → errorCode 7; a range wider than
  // kPdfMaxPagesPerRead → errorCode 8.
  if (input.pages.has_value()) {
    int first_page = 0;
    int last_page = 0;
    if (!ParsePdfPageRange(*input.pages, &first_page, &last_page)) {
      base::DictValue params;
      params.Set("pages", *input.pages);
      std::move(done).Reject("invalid_pages", std::move(params));
      return;
    }
    const int range_size =
        last_page == std::numeric_limits<int>::max()
            ? kPdfMaxPagesPerRead + 1
            : last_page - first_page + 1;
    if (range_size > kPdfMaxPagesPerRead) {
      base::DictValue params;
      params.Set("pages", *input.pages);
      params.Set("max_pages", kPdfMaxPagesPerRead);
      std::move(done).Reject("too_many_pages", std::move(params));
      return;
    }
  }

  // Spec 2-1-5 errorCode 9: a path-based check (no I/O) for device files that would hang
  // the read. Safe devices like /dev/null are intentionally allowed.
  if (IsBlockedDevicePath(input.file_path)) {
    base::DictValue params;
    params.Set("path", input.file_path);
    std::move(done).Reject("blocked_device_path", std::move(params));
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

  BlockingRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&FileTool::ReadFileBlocking, std::move(path), input.offset,
                     input.limit, input.pages, input.context_window,
                     std::move(last_read)),
      base::BindOnce(&FileTool::OnReadDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done)));
}

void FileTool::RunWriteOnUI(::nexus::generated::FilesWriteInput input,
                            ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    base::DictValue params;
    params.Set("reason", "file_path_not_absolute");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  // Spec 2-3-4 row 1: a network share path is refused before any I/O.
  // On Windows, merely probing `\\host\share` (base::PathExists and
  // friends) makes the OS authenticate to that host, handing it the
  // user's hashed login.
  if (base::StartsWith(input.file_path, "\\\\",
                       base::CompareCase::SENSITIVE) ||
      base::StartsWith(input.file_path, "//", base::CompareCase::SENSITIVE)) {
    base::DictValue params;
    params.Set("reason", "unc_path");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  std::optional<FileTool::ReadState> last_read;
  auto read_it = read_state_.find(path);
  if (read_it != read_state_.end()) {
    last_read = read_it->second;
  }

  BlockingRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&FileTool::WriteFileBlocking, std::move(path),
                     std::move(input.content), std::move(last_read)),
      base::BindOnce(&FileTool::OnWriteDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done)));
}

void FileTool::RunEditOnUI(::nexus::generated::FilesEditInput input,
                           ToolCallback done) {
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  // Edit's pre-flight has more invariants than read/write.
  if (!path.IsAbsolute()) {
    base::DictValue params;
    params.Set("reason", "file_path_not_absolute");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }
  if (input.old_string == input.new_string) {
    base::DictValue params;
    params.Set("reason", "same_strings");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }
  // Spec 2-2-4 code 5: .ipynb files must use NotebookEdit.
  if (LowercaseExtension(path) == "ipynb") {
    base::DictValue params;
    std::move(done).Reject("notebook_file", std::move(params));
    return;
  }

  bool replace_all = input.replace_all.value_or(false);
  std::optional<FileTool::ReadState> last_read;
  auto read_it = read_state_.find(path);
  if (read_it != read_state_.end()) {
    last_read = read_it->second;
  }

  BlockingRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&FileTool::EditFileBlocking, std::move(path),
                     std::move(input.old_string), std::move(input.new_string),
                     replace_all, std::move(last_read)),
      base::BindOnce(&FileTool::OnEditDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(done)));
}

void FileTool::RunGlobOnUI(::nexus::generated::FilesGlobInput input,
                           ToolCallback done) {
  if (input.pattern.empty()) {
    base::DictValue params;
    params.Set("reason", "pattern_required");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  // Reading the session cwd is safe here because we're on UI — the
  // ToolSession owning us is alive.
  ToolSession* session = GetSession();
  base::FilePath default_root = session ? session->cwd() : base::FilePath();

  // Resolve the search root + the ripgrep `--glob` pattern: an ABSOLUTE `pattern` is split into its static
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
    base::DictValue params;
    params.Set("reason", "no_cwd");
    std::move(done).Reject("invalid_args", std::move(params));
    return;
  }

  int limit = input.limit.value_or(kDefaultGlobLimit);
  if (limit <= 0) {
    limit = kDefaultGlobLimit;
  }

  // `rg --files --glob <pattern> --sort=modified --no-ignore --hidden
  // <target>`. `--no-ignore` + `--hidden` make Glob find gitignored and hidden
  // files too; `--sort=modified`
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
  // Spec 1-1-3 / 2-1-4: the token budget is a FIXED 25000 and does not
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
      result.error_params.Set("path", path.AsUTF8Unsafe());
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

  // Spec 2-1-5: a directory is not a readable
  // file. Report it distinctly so the model switches to a listing tool instead
  // of retrying, rather than collapsing to the generic "failed to read file".
  if (have_file_info && file_info.is_directory) {
    result.error_code = "is_directory";
    result.error_params.Set("path", path.AsUTF8Unsafe());
    return result;
  }

  if (!limit.has_value() && have_file_info && !file_info.is_directory &&
      file_info.size > kDefaultReadMaxSizeBytes) {
    result.error_code = "file_too_large";
    result.error_params.Set("size_bytes", static_cast<double>(file_info.size));
    result.error_params.Set("limit_bytes",
                            static_cast<double>(kDefaultReadMaxSizeBytes));
    return result;
  }

  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    // Spec 2-1-5: a missing file gets a distinct
    // "File does not exist." signal so the model corrects the path instead of
    // treating it as an opaque I/O failure.
    if (!base::PathExists(path)) {
      result.error_code = "file_not_found";
      return result;
    }
    result.error_code = "read_failed";
    result.error_params.Set("path", path.AsUTF8Unsafe());
    return result;
  }

  // Spec 2-1-6: detect BOM-based encoding and transcode
  // UTF-16LE → UTF-8 so the model sees readable text (and so the binary
  // heuristic below doesn't trip on UTF-16LE's null bytes). `contents` keeps
  // the RAW on-disk bytes for the read_state_ cache + dedup/staleness symmetry;
  // `text` is the UTF-8 view used for projection + token estimation.
  bool is_utf16le = false;
  const std::string text = DecodeToUTF8(contents, &is_utf16le);

  if (!is_utf16le && net::LooksLikeBinary(contents)) {
    result.error_code = "unsupported_binary_file";
    return result;
  }

  if (last_read.has_value() && last_read->from_read &&
      last_read->offset == offset && last_read->limit == limit &&
      !pages.has_value() && !last_modified.is_null() &&
      last_modified == last_read->last_modified &&
      contents == last_read->content) {
    result.ok = true;
    result.notice.Set("kind", "unchanged");
    return result;
  }

  int start_line = offset.value_or(1);
  if (start_line < 1) {
    start_line = 1;
  }
  // Spec 2-1-9 / 2-1-10: the 2000-line figure in the description is guidance
  // and is NOT enforced. The only line cap is the caller's explicit `limit`; with no limit
  // the whole file from `start` is projected (overflow is caught by the
  // post-read token guard, which throws rather than truncating).
  std::optional<int> max_lines;
  if (limit.has_value() && *limit >= 1) {
    max_lines = *limit;
  }

  // Strip a leading U+FEFF before line-splitting (spec 2-1-6):
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

  // Empty selection: return a warning in place of content, never an empty
  // result, distinguishing an empty file from an offset past EOF.
  if (projection.lines_emitted == 0) {
    result.ok = true;
    result.file_path = std::move(path);
    result.full_content = contents;
    result.last_modified = last_modified;
    result.offset = offset;
    result.limit = limit;
    // Spec 2-1-8 / 2-1-10 inv.4: Read NEVER sets isPartialView — a
    // partial read does not block a later Edit/Write (the staleness guard is
    // the content comparison, not the read's range).
    result.cache_read_state = true;
    if (projection.total_lines == 0) {
      result.notice.Set("kind", "empty");
    } else {
      result.notice.Set("kind", "short");
      result.notice.Set("offset", start_line);
      result.notice.Set("total_lines", projection.total_lines);
    }
    return result;
  }

  const int token_estimate = EstimateReadTokens(projection.content, ext);
  if (token_estimate > read_budget.max_output_tokens) {
    result.error_code = "max_file_read_token_exceeded";
    result.error_params.Set("tokens", token_estimate);
    result.error_params.Set("max_tokens", read_budget.max_output_tokens);
    return result;
  }

  // Read returns the line-numbered content directly as the result text
  // (spec 2-1-7). No truncation footer is appended — Read never silently
  // truncates; overflow
  // is enforced by the token guard above, which throws (2-1-4.5).
  result.ok = true;
  result.file_path = std::move(path);
  result.full_content = contents;
  result.last_modified = last_modified;
  result.offset = offset;
  result.limit = limit;
  // Spec 2-1-8 / 2-1-10 inv.4: Read never sets isPartialView.
  result.cache_read_state = true;
  result.content = std::move(projection.projection);
  return result;
}

// static
FileTool::WriteResult FileTool::WriteFileBlocking(
    base::FilePath path,
    std::string content,
    std::optional<FileTool::ReadState> last_read) {
  WriteResult result;

  // Probe existence BEFORE we touch the file so the prose response can
  // distinguish create-vs-update (spec 2-3-5). base::WriteFile overwrites on create, so without a
  // prior probe we couldn't tell which case applies.
  const bool was_create = !base::PathExists(path);

  // Spec 2-3-4: overwriting an existing file requires a previous full Read and a
  // matching on-disk content snapshot. New files do not require a prior Read.
  // The single read below serves BOTH the staleness guard and the UI-diff
  // snapshot — reading twice would be a TOCTOU window where a concurrent
  // writer slips content past the guard.
  std::string original_contents;
  bool have_original = false;
  if (!was_create) {
    if (!last_read.has_value() || last_read->is_partial_view) {
      result.error_code = "file_not_read";
      result.error_params.Set("phase", "write");
      return result;
    }
    have_original = base::ReadFileToString(path, &original_contents);
    const bool content_unchanged =
        have_original && original_contents == last_read->content;
    if (!content_unchanged) {
      result.error_code = "file_modified_since_read";
      return result;
    }
  }

  // Ensure the parent directory exists. base::WriteFile fails silently
  // (returns false) when the parent is missing; creating it explicitly
  // lets the agent write a file under a not-yet-existing directory tree
  // (spec 2-3-5).
  base::FilePath parent = path.DirName();
  if (!parent.empty() && !base::DirectoryExists(parent)) {
    if (!base::CreateDirectory(parent)) {
      result.error_code = "write_failed";
      result.error_params.Set("reason", "parent_dir");
      result.error_params.Set("path", parent.AsUTF8Unsafe());
      return result;
    }
  }

  // Spec 2-3-7: Write ALWAYS outputs LF. The model sends explicit
  // line endings; do not preserve the old file's CRLF (contrast Edit which
  // restores original line endings via RestoreCRLF).
  content = NormalizeCRLF(content);

  // Spec 2-3-7.2: preserve the original file's BOM-based encoding.
  // The write bytes may differ from |content| (e.g. UTF-16LE transcoding),
  // but ReadState stores the raw on-disk bytes so the staleness check
  // (raw-vs-raw) stays symmetric with Read.
  std::string write_bytes = content;
  if (have_original) {
    write_bytes = PreserveOriginalEncoding(original_contents, content);
  }

  if (!base::WriteFile(path, write_bytes)) {
    result.error_code = "write_failed";
    result.error_params.Set("path", path.AsUTF8Unsafe());
    return result;
  }

  // The result is an outcome code plus the byte count; the runtime turns it
  // into the model-facing sentence. The byte count matters: a result that
  // only confirmed the file existed left the model re-issuing Write to
  // verify its content landed. Echoing the path lets the model compare what
  // it sent with what was written in one step.
  result.ok = true;
  result.outcome = was_create ? "created" : "updated";
  result.bytes_written = static_cast<int64_t>(write_bytes.size());
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

  // Spec 2-2-4 code 10: reject files > 1 GiB to prevent OOM.
  base::File::Info pre_info;
  if (base::GetFileInfo(path, &pre_info) &&
      pre_info.size > kMaxEditFileSize) {
    result.error_code = "file_too_large";
    result.error_params.Set("kind", "edit");
    result.error_params.Set("size_bytes", static_cast<double>(pre_info.size));
    result.error_params.Set("limit_bytes",
                            static_cast<double>(kMaxEditFileSize));
    return result;
  }

  // Spec 2-2-4 codes 4/7: file creation path — old_string empty
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
        result.error_params.Set("path", path.AsUTF8Unsafe());
        return result;
      }
      result.ok = true;
      result.outcome = "updated";
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
    result.error_params.Set("cwd", path.DirName().AsUTF8Unsafe());
    return result;
  }

  // Spec 2-2-4 code 6: file must have been fully read first.
  if (!last_read.has_value()) {
    result.error_code = "file_not_read";
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
    result.error_params.Set("phase", "partial");
    result.error_params.Set("offset", offset_str);
    result.error_params.Set("limit", limit_str);
    return result;
  }

  // Spec 2-2-4 code 3: empty old_string on existing non-empty
  // file is a create-over-existing error.
  if (old_string.empty()) {
    std::string existing;
    base::ReadFileToString(path, &existing);
    if (!existing.empty() &&
        base::TrimWhitespaceASCII(existing, base::TRIM_ALL).size() > 0) {
      result.error_code = "file_already_exists";
      return result;
    }
    // Empty file with empty old_string: replace empty with new_string.
    if (!base::WriteFile(path, new_string)) {
      result.error_code = "write_failed";
      result.error_params.Set("path", path.AsUTF8Unsafe());
      return result;
    }
    result.ok = true;
    result.outcome = "updated";
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
    result.error_params.Set("path", path.AsUTF8Unsafe());
    result.error_params.Set("phase", "edit");
    return result;
  }

  // Spec 2-2-4 code 7: staleness check. Compare raw bytes first
  // (symmetric with read_state_ which stores raw). If raw differs, the file
  // was modified since read.
  if (contents != last_read->content) {
    result.error_code = "file_modified_since_read";
    return result;
  }

  // Spec 2-2-8: decode BOM-based encoding to UTF-8
  // for matching. The model's old_string/new_string are UTF-8; a UTF-16LE file
  // must be transcoded before substring matching can work. `contents` keeps the
  // RAW bytes for the read_state_ cache + re-encode on write; `work_content`
  // starts as the UTF-8 view.
  bool is_utf16le = false;
  std::string work_content = DecodeToUTF8(contents, &is_utf16le);

  // Spec 2-2-8: normalize CRLF → LF for matching. The model sees
  // LF-normalized content from Read (the projection strips \r), so old_string
  // will be LF. The file on disk may be CRLF.
  const bool is_crlf = HasCRLF(work_content);
  if (is_crlf) {
    work_content = NormalizeCRLF(work_content);
  }

  // Spec 2-2-6: strip trailing whitespace
  // from new_string for non-markdown files.
  const std::string ext = LowercaseExtension(path);
  const bool is_markdown = (ext == "md" || ext == "mdx");
  std::string work_new_string =
      is_markdown ? new_string : StripTrailingWhitespace(new_string);

  // Spec 2-2-6: exact match first, then
  // quote-normalized fallback.
  bool found = false;
  std::string actual_old_string =
      FindActualString(work_content, old_string, &found);

  // Spec 2-2-6: if the
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
    result.error_params.Set("old_string", old_string);
    return result;
  }

  // Spec 2-2-6: when the file uses curly quotes
  // and the model sent straight quotes, apply curly style to new_string.
  work_new_string =
      PreserveQuoteStyle(old_string, actual_old_string, work_new_string);

  // Spec 2-2-4 code 9: uniqueness check.
  const size_t occurrences =
      CountOccurrences(work_content, actual_old_string);
  if (occurrences > 1 && !replace_all) {
    result.error_code = "ambiguous_match";
    result.error_params.Set("count", static_cast<int>(occurrences));
    result.error_params.Set("old_string", old_string);
    return result;
  }

  // Spec 2-2-6: apply with delete-strips-newline.
  // `updated` is the UTF-8, LF-normalized post-edit content. `work_content`
  // still holds the decoded + LF-normalized PRE-edit content (what old_string
  // matched against).
  std::string updated = ApplyEditToFile(work_content, actual_old_string,
                                        work_new_string, replace_all);
  if (updated == work_content) {
    result.error_code = "no_match";
    result.error_params.Set("reason", "apply");
    return result;
  }

  // Spec 2-2-8: restore original line endings, then re-encode to the
  // original BOM-based encoding before writing. `write_bytes` is what lands on
  // disk — for UTF-16LE this re-introduces the BOM + little-endian code units.
  if (is_crlf) {
    updated = RestoreCRLF(updated);
  }
  const std::string write_bytes =
      is_utf16le ? PreserveOriginalEncoding(contents, updated) : updated;

  if (!base::WriteFile(path, write_bytes)) {
    result.error_code = "write_failed";
    result.error_params.Set("path", path.AsUTF8Unsafe());
    result.error_params.Set("reason", "write_back");
    return result;
  }

  // Outcome code; the runtime renders the model-facing sentence.
  result.ok = true;
  result.outcome = replace_all ? "updated_all" : "updated";
  // Carry the pre-edit content on a UI-only side channel (see OnEditDone) so the
  // browser can render a file-relative diff. It never reaches the model. This is
  // the decoded + LF-normalized view (what old_string matched), so the JS diff
  // works for UTF-16LE and CRLF alike — the diff is of the LF-normalized
  // content.
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
                           std::move(result.error_params));
    return;
  }
  if (!result.content.empty() || !result.notice.empty()) {
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
    if (!result.content.empty()) {
      std::move(done).Resolve(base::Value(std::move(result.content)));
      return;
    }
    base::DictValue dict;
    dict.Set("notice", std::move(result.notice));
    std::move(done).Resolve(std::move(dict));
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
    base::DictValue params;
    // The media service names its own failure; `detail` is its sentence and
    // `source` tells the runtime to show that sentence, not a files one.
    params.Set("path", path.AsUTF8Unsafe());
    params.Set("source", "media");
    if (error_message.has_value()) {
      params.Set("detail", *error_message);
    }
    std::move(done).Reject(error_code.value_or("media_read_failed"),
                           std::move(params));
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

  // Spec 2-1-8: give notebook reads a native read-state entry so a subsequent Edit/Write
  // on the .ipynb passes the "file has been read" precondition. Only notebooks
  // populate `full_text_for_cache`; images/PDFs leave it null and record
  // no read-state (they aren't text-editable). `last_modified`
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
  base::DictValue media;
  media.Set("kind", "media");
  media.Set("path", path.AsUTF8Unsafe());
  payload.Set("media", std::move(media));
  payload.Set("content_parts", std::move(parts));
  std::move(done).Resolve(base::Value(std::move(payload)));
}

void FileTool::OnWriteDone(ToolCallback done, WriteResult result) {
  if (!result.ok) {
    std::move(done).Reject(std::move(result.error_code),
                           std::move(result.error_params));
    return;
  }
  // On success, forward `{ outcome, path, bytes?, original_file? }`; the
  // runtime renders the sentence the model reads and uses `original_file`
  // — present only when overwriting — for the file-relative diff.
  if (!result.file_path.empty()) {
    ReadState state;
    state.content = result.updated_content;
    state.last_modified = result.last_modified;
    state.is_partial_view = false;
    read_state_[result.file_path] = std::move(state);
  }
  if (!result.outcome.empty()) {
    base::DictValue dict;
    dict.Set("outcome", std::move(result.outcome));
    dict.Set("path", result.file_path.AsUTF8Unsafe());
    if (result.bytes_written > 0) {
      dict.Set("bytes", static_cast<double>(result.bytes_written));
    }
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
                           std::move(result.error_params));
    return;
  }
  // On success, forward a structured dict `{ message, original_file }`. The JS
  // hostedTools.ts pulls `message` back out as the model-facing prose (so the
  // model sees the sentence, never the JSON) and uses
  // `original_file` to render a file-relative diff. See OnWriteDone.
  if (!result.file_path.empty()) {
    ReadState state;
    state.content = result.updated_content;
    state.last_modified = result.last_modified;
    state.is_partial_view = false;
    read_state_[result.file_path] = std::move(state);
  }
  if (!result.outcome.empty()) {
    base::DictValue dict;
    dict.Set("outcome", std::move(result.outcome));
    dict.Set("path", result.file_path.AsUTF8Unsafe());
    if (result.bytes_written > 0) {
      dict.Set("bytes", static_cast<double>(result.bytes_written));
    }
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

void FileTool::CompleteGlobForTesting(ToolCallback done,
                                      RipgrepRunResult result) {
  OnGlobRgDone(std::move(done), /*limit=*/0, std::move(result));
}

void FileTool::OnGlobRgDone(ToolCallback done,
                            int limit,
                            RipgrepRunResult result) {
  // Pre-launch failure: rg was unavailable and the runner's one-shot
  // auto-install didn't recover it. The runner already shaped a diagnostic
  // message (+ manual-install hint); surface it under a Glob-labeled code.
  if (!result.launched) {
    std::move(done).Reject(
        "glob_failed",
        ServiceFailureParams(result.error_code, result.error_message,
                             std::nullopt));
    return;
  }

  std::vector<std::string>& lines = result.lines;

  // "No files found" on empty, otherwise the matched paths joined by '\n' with a truncation
  // caveat when there were more than `limit`. The model uses an anchored Glob
  // (the exact path as the pattern) as a file-exists probe and keys off the
  // "No files found" sentinel — rg exits 1 (no matches) → empty lines here.
  bool truncated = static_cast<int>(lines.size()) > limit;
  if (truncated) {
    lines.resize(limit);
  }
  base::ListValue files;
  for (std::string& line : lines) {
    files.Append(std::move(line));
  }
  base::DictValue dict;
  dict.Set("files", std::move(files));
  dict.Set("truncated", truncated);
  std::move(done).Resolve(std::move(dict));
}

}  // namespace nexus

