#include "custom_browser/browser/nexus/api/fs/nexus_fs_bridge_adapter.h"

#include <stdint.h>

#include <algorithm>
#include <cinttypes>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

// ---- Wire-contract limits for the batched routes --------------------------
//
// These bound memory and keep the thread-pool task short (a longer task
// widens the adapter-teardown window). They are NOT sized to fit the guest's
// 30 s fs timeout — at these numbers that has an order of magnitude of
// headroom.

// Over-cap is an InvalidArgs, never a silent truncation: a short answer to an
// existence check would mislabel real files as missing. Calibrated against
// the network capability's 2000-char URL cap; the biggest real caller
// (file-path affordances in a transcript) sees tens.
constexpr size_t kMaxStatBatchPaths = 2000;

// Depth 1 == plain readdir, so a caller who omits the arg gets the cheap
// thing rather than a surprise tree walk. The session-store scan asks for 3.
constexpr int32_t kDefaultTreeDepth = 1;
constexpr int32_t kMaxTreeDepth = 8;

// ~10x the real profile's ~2000 rows at depth 3. 20000 rows is ~1.8 MB of
// JSON; 50000 is where a caller should be using a different tool.
constexpr int32_t kDefaultTreeEntries = 20000;
constexpr int32_t kMaxTreeEntries = 50000;

constexpr size_t kMaxReadBatchItems = 512;

// One pathological transcript must not eat the whole batch budget. The
// sidebar never asks for more than 64 KiB per item.
constexpr size_t kMaxReadBatchItemBytes = 8u * 1024 * 1024;

// Worst realistic case is 226 legacy sessions x 64 KiB = 14.5 MiB; this is
// 2x headroom and far under NexusFsHost's 256 MiB single-range guard.
constexpr size_t kMaxReadBatchTotalBytes = 32u * 1024 * 1024;

int32_t ClampTreeBound(std::optional<int32_t> requested,
                       int32_t fallback,
                       int32_t ceiling) {
  const int32_t value = requested.value_or(0);
  if (value <= 0) {
    return fallback;
  }
  return value > ceiling ? ceiling : value;
}

// ---------------------------------------------------------------------------
// Reply shapers — run on UI after the thread-pool work completes.
//
// Each takes the typed result by value (PostTaskAndReplyWithResult appends
// it as the trailing arg) and forwards it through the BridgeResponseCallback.
// They are free functions (not adapter members) and touch only `done` +
// `result`, never the adapter — so they're safe to run even if the adapter
// was destroyed while the work was in flight. `done` itself weak-guards the
// owning handler, so the response is dropped cleanly in that case.
// ---------------------------------------------------------------------------

void RespondString(BridgeResponseCallback done, std::string value) {
  std::move(done).Success(Value(std::move(value)));
}

template <typename JsonResult>
void RespondJson(BridgeResponseCallback done,
                 const char* error_label,
                 const char* decode_label,
                 JsonResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : error_label);
    return;
  }
  std::optional<Value> value =
      base::JSONReader::Read(result.json, base::JSON_PARSE_RFC);
  if (!value.has_value()) {
    std::move(done).Error("FS_ERROR", decode_label);
    return;
  }
  std::move(done).Success(std::move(*value));
}

void RespondReadResult(BridgeResponseCallback done,
                       const char* error_label,
                       NexusFsHost::ReadResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : error_label);
    return;
  }
  std::move(done).Binary(
      Value(),
      result.data.has_value() ? std::move(*result.data) : mojo_base::BigBuffer(),
      "$result");
}

void RespondReadRange(BridgeResponseCallback done,
                      NexusFsHost::ReadRangeResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "ReadRange failed");
    return;
  }
  Dict envelope;
  envelope.Set("bytesRead", static_cast<int>(result.bytes_read));
  std::move(done).Binary(
      Value(std::move(envelope)),
      result.data.has_value() ? std::move(*result.data) : mojo_base::BigBuffer(),
      "data");
}

// ---- Batched shapers ------------------------------------------------------
//
// These three take base::ListValue straight from the host rather than a JSON
// string. RespondJson's write-then-reparse is free for one stat row and
// wasteful for the ~2000 a session-store scan returns.

void RespondStatBatch(BridgeResponseCallback done,
                      NexusFsHost::StatBatchResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "StatBatch failed");
    return;
  }
  std::move(done).Success(Value(std::move(result.rows)));
}

void RespondReaddirTree(BridgeResponseCallback done,
                        NexusFsHost::ReaddirTreeResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "ReaddirTree failed");
    return;
  }
  Dict envelope;
  envelope.Set("root", std::move(result.root));
  envelope.Set("truncated", result.truncated);
  envelope.Set("entries", Value(std::move(result.entries)));
  std::move(done).Success(Value(std::move(envelope)));
}

// The index rides the JSON envelope, every item's bytes ride ONE buffer on
// the response data port — a response can carry only a single buffer, so a
// list of per-item buffers is not expressible. Same shape as
// RespondReadRange, with a list instead of a scalar.
void RespondReadBatch(BridgeResponseCallback done,
                      NexusFsHost::ReadBatchResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "ReadBatch failed");
    return;
  }
  const size_t total = result.data.has_value() ? result.data->size() : 0;
  Dict envelope;
  envelope.Set("entries", Value(std::move(result.entries)));
  envelope.Set("totalBytes", static_cast<double>(total));
  envelope.Set("truncated", result.truncated);
  std::move(done).Binary(
      Value(std::move(envelope)),
      result.data.has_value() ? std::move(*result.data) : mojo_base::BigBuffer(),
      "blob");
}

void RespondSimple(BridgeResponseCallback done,
                   const char* error_label,
                   NexusFsHost::SimpleResult result) {
  if (!result.success) {
    std::move(done).Error(
        !result.error_code.empty() ? result.error_code : "FS_ERROR",
        !result.error.empty() ? result.error : error_label);
    return;
  }
  std::move(done).SuccessVoid();
}

// fs.writeFile with ifMatch: PRECONDITION_FAILED is its own code (the guest
// branches on it), and a success carries the new stat.
void RespondWriteIfMatch(BridgeResponseCallback done,
                         NexusFsHost::WriteIfMatchResult result) {
  if (!result.success) {
    std::move(done).Error(
        !result.error_code.empty() ? result.error_code : "FS_ERROR",
        !result.error.empty() ? result.error : "WriteFile failed");
    return;
  }
  Dict envelope;
  envelope.Set("ok", true);
  envelope.Set("mtimeMs", result.mtime_ms);
  envelope.Set("size", result.size);
  std::move(done).Success(Value(std::move(envelope)));
}

void RespondReadlink(BridgeResponseCallback done,
                     NexusFsHost::ReadlinkResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Readlink failed");
    return;
  }
  Dict envelope;
  envelope.Set("target", result.target);
  std::move(done).Success(Value(std::move(envelope)));
}

void RespondChmod(BridgeResponseCallback done,
                  NexusFsHost::ChmodResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Chmod failed");
    return;
  }
  Dict envelope;
  envelope.Set("applied", result.applied);
  std::move(done).Success(Value(std::move(envelope)));
}

void RespondExists(BridgeResponseCallback done,
                   NexusFsHost::ExistsResult result) {
  if (!result.success && !result.error.empty()) {
    std::move(done).Error("FS_ERROR", result.error);
    return;
  }
  std::move(done).Success(Value(result.exists));
}

void RespondRealpath(BridgeResponseCallback done,
                     NexusFsHost::RealpathResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Realpath failed");
    return;
  }
  Dict envelope;
  envelope.Set("path", result.path);
  std::move(done).Success(Value(std::move(envelope)));
}

void RespondSystemInfo(BridgeResponseCallback done,
                       NexusFsHost::SystemInfoResult result) {
  // Always Success (never Error) — empty fields mean "unknown" and the
  // guest drops the corresponding `# Environment` line.
  Dict envelope;
  envelope.Set("platform", result.platform);
  envelope.Set("osVersion", result.os_version);
  envelope.Set("shell", result.shell);
  std::move(done).Success(Value(std::move(envelope)));
}

void RespondPaths(BridgeResponseCallback done,
                  NexusFsHost::PathsResult result) {
  // Always Success: an empty field means "not resolvable here".
  Dict envelope;
  envelope.Set("home", result.home);
  envelope.Set("temp", result.temp);
  envelope.Set("appData", result.app_data);
  envelope.Set("cache", result.cache);
  std::move(done).Success(Value(std::move(envelope)));
}

}  // namespace

NexusFsBridgeAdapter::NexusFsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    const base::FilePath& cwd)
    : ::nexus::generated::FsApiBase(delegate),
      fs_host_(base::MakeRefCounted<NexusFsHost>(cwd)) {}

NexusFsBridgeAdapter::~NexusFsBridgeAdapter() = default;

// ===========================================================================
// Per-action entry points (runtime.thread = "ui")
//
// The codegen dispatcher invokes each on UI with a fully-populated typed
// input (inline-vs-port resolution for `bytes` already done). Each body hops
// the blocking fs work to base::ThreadPool via PostTaskAndReplyWithResult,
// binding the work to a `scoped_refptr<NexusFsHost>` — so the worker holds
// the host alive and NEVER touches this adapter. The reply shaper runs back
// on UI and delivers via `done` (weak-guarded). A port-service teardown that
// destroys this adapter mid-op therefore can't use-after-free it: the worker
// finishes against the still-refcounted host, and the reply drops cleanly.
// See docs/agents/nexus-tools-api.md §6.
//
// Traits: {MayBlock(), USER_VISIBLE} — the guest is waiting on the result.
// ===========================================================================

void NexusFsBridgeAdapter::HandleCwdOnUI(
    ::nexus::generated::CwdInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Cwd, fs_host_),
      base::BindOnce(&RespondString, std::move(done)));
}

void NexusFsBridgeAdapter::HandleUserHomeOnUI(
    ::nexus::generated::UserHomeInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::UserHome, fs_host_),
      base::BindOnce(&RespondString, std::move(done)));
}

void NexusFsBridgeAdapter::HandleStatOnUI(
    ::nexus::generated::StatInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Stat, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondJson<NexusFsHost::StatResult>, std::move(done),
                     "Stat failed", "Failed to decode stat response"));
}

void NexusFsBridgeAdapter::HandleReaddirOnUI(
    ::nexus::generated::ReaddirInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Readdir, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondJson<NexusFsHost::ReaddirResult>, std::move(done),
                     "Readdir failed", "Failed to decode readdir response"));
}

void NexusFsBridgeAdapter::HandleStatBatchOnUI(
    ::nexus::generated::StatBatchInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.paths.size() > kMaxStatBatchPaths) {
    std::move(done).InvalidArgs("statBatch: too many paths");
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::StatBatch, fs_host_, std::move(input.paths)),
      base::BindOnce(&RespondStatBatch, std::move(done)));
}

void NexusFsBridgeAdapter::HandleReaddirTreeOnUI(
    ::nexus::generated::ReaddirTreeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Clamp on UI, where we can still answer InvalidArgs; the host takes the
  // bounds as given.
  const int32_t depth =
      ClampTreeBound(input.maxDepth, kDefaultTreeDepth, kMaxTreeDepth);
  const int32_t entries =
      ClampTreeBound(input.maxEntries, kDefaultTreeEntries, kMaxTreeEntries);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::ReaddirTree, fs_host_,
                     std::move(input.path), depth, entries),
      base::BindOnce(&RespondReaddirTree, std::move(done)));
}

void NexusFsBridgeAdapter::HandleReadBatchOnUI(
    ::nexus::generated::ReadBatchInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.items.size() > kMaxReadBatchItems) {
    std::move(done).InvalidArgs("readBatch: too many items");
    return;
  }

  // Convert base::Value -> typed items HERE, on UI, so the worker task never
  // touches base::Value. A malformed entry is rejected rather than skipped:
  // the codegen array<object> extractor already drops non-dicts, and a
  // silently shorter list would shift every later index and attribute bytes
  // to the wrong file.
  std::vector<NexusFsHost::ReadBatchItem> items;
  items.reserve(input.items.size());
  for (const base::DictValue& raw : input.items) {
    const std::string* path = raw.FindString("path");
    if (!path || path->empty()) {
      std::move(done).InvalidArgs("readBatch: item is missing \"path\"");
      return;
    }
    NexusFsHost::ReadBatchItem item;
    item.path = *path;
    item.offset = static_cast<uint64_t>(std::max(0, raw.FindInt("offset").value_or(0)));
    item.length = static_cast<uint32_t>(std::max(0, raw.FindInt("length").value_or(0)));
    items.push_back(std::move(item));
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::ReadBatch, fs_host_, std::move(items),
                     kMaxReadBatchTotalBytes, kMaxReadBatchItemBytes),
      base::BindOnce(&RespondReadBatch, std::move(done)));
}

void NexusFsBridgeAdapter::HandleReadFileOnUI(
    ::nexus::generated::ReadFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::ReadFile, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondReadResult, std::move(done), "ReadFile failed"));
}

void NexusFsBridgeAdapter::HandleReadFileBytesOnUI(
    ::nexus::generated::ReadFileBytesInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const int32_t max_bytes = std::max(0, input.maxBytes.value_or(0));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](scoped_refptr<NexusFsHost> host, std::string path,
             int32_t max_bytes) {
            // max_bytes > 0 → bounded prefix read; else → whole file. Both
            // surface as a ReadResult so the reply shaper is uniform.
            if (max_bytes > 0) {
              NexusFsHost::ReadRangeResult range = host->ReadRange(
                  path, /*offset=*/0, base::checked_cast<uint32_t>(max_bytes));
              NexusFsHost::ReadResult out;
              out.success = range.success;
              out.error = std::move(range.error);
              out.data = std::move(range.data);
              return out;
            }
            return host->ReadFile(path);
          },
          fs_host_, std::move(input.path), max_bytes),
      base::BindOnce(&RespondReadResult, std::move(done),
                     "ReadFileBytes failed"));
}

void NexusFsBridgeAdapter::HandleReadRangeOnUI(
    ::nexus::generated::ReadRangeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const uint64_t offset = base::checked_cast<uint64_t>(
      std::max<int32_t>(0, input.offset.value_or(0)));
  const uint32_t length = base::checked_cast<uint32_t>(
      std::max<int32_t>(0, input.length.value_or(0)));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::ReadRange, fs_host_, std::move(input.path),
                     offset, length),
      base::BindOnce(&RespondReadRange, std::move(done)));
}

void NexusFsBridgeAdapter::HandleWriteFileOnUI(
    ::nexus::generated::WriteFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // input.content is always populated here — the codegen dispatcher resolved
  // either inline-payload or transferred-port before invoking. Build the
  // BigBuffer on the worker to avoid a copy on UI.
  // `atomic` is an additive, ignore-if-absent input (no interface bump): a
  // guest that does not send it gets the in-place write it always got.
  const bool atomic = input.atomic.value_or(false);
  if (input.ifMatch.has_value()) {
    // Compare-and-swap (additive; advertised as fs.writeFile.ifMatch in the
    // manifest because an older host would ignore the field and write).
    const std::optional<double> mtime_ms = input.ifMatch->FindDouble("mtimeMs");
    const std::optional<double> size = input.ifMatch->FindDouble("size");
    if (!mtime_ms.has_value() || !size.has_value()) {
      std::move(done).InvalidArgs(
          "writeFile: ifMatch needs numeric mtimeMs and size");
      return;
    }
    NexusFsHost::WriteIfMatch if_match;
    if_match.mtime_ms = *mtime_ms;
    if_match.size = *size;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](scoped_refptr<NexusFsHost> host, std::string path,
               std::vector<uint8_t> content, bool atomic,
               NexusFsHost::WriteIfMatch if_match) {
              return host->WriteFileIfMatch(
                  path, mojo_base::BigBuffer(base::span(content)), atomic,
                  if_match);
            },
            fs_host_, std::move(input.path), std::move(input.content), atomic,
            if_match),
        base::BindOnce(&RespondWriteIfMatch, std::move(done)));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](scoped_refptr<NexusFsHost> host, std::string path,
             std::vector<uint8_t> content, bool atomic) {
            return host->WriteFile(
                path, mojo_base::BigBuffer(base::span(content)), atomic);
          },
          fs_host_, std::move(input.path), std::move(input.content), atomic),
      base::BindOnce(&RespondSimple, std::move(done), "WriteFile failed"));
}

void NexusFsBridgeAdapter::HandleAppendFileOnUI(
    ::nexus::generated::AppendFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // input.data is always populated here (see writeFile).
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](scoped_refptr<NexusFsHost> host, std::string path,
             std::vector<uint8_t> data) {
            return host->AppendFile(path,
                                    mojo_base::BigBuffer(base::span(data)));
          },
          fs_host_, std::move(input.path), std::move(input.data)),
      base::BindOnce(&RespondSimple, std::move(done), "AppendFile failed"));
}

void NexusFsBridgeAdapter::HandleMkdirOnUI(
    ::nexus::generated::MkdirInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Mkdir, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondSimple, std::move(done), "Mkdir failed"));
}

void NexusFsBridgeAdapter::HandleRenameOnUI(
    ::nexus::generated::RenameInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Rename, fs_host_,
                     std::move(input.oldPath), std::move(input.newPath)),
      base::BindOnce(&RespondSimple, std::move(done), "Rename failed"));
}

void NexusFsBridgeAdapter::HandleRmOnUI(
    ::nexus::generated::RmInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Rm, fs_host_, std::move(input.path),
                     input.recursive.value_or(false),
                     input.force.value_or(false)),
      base::BindOnce(&RespondSimple, std::move(done), "Rm failed"));
}

void NexusFsBridgeAdapter::HandleUnlinkOnUI(
    ::nexus::generated::UnlinkInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Unlink, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondSimple, std::move(done), "Unlink failed"));
}

void NexusFsBridgeAdapter::HandleExistsOnUI(
    ::nexus::generated::ExistsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Exists, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondExists, std::move(done)));
}

void NexusFsBridgeAdapter::HandleRealpathOnUI(
    ::nexus::generated::RealpathInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // `resolveLinks` is an additive, ignore-if-absent input (no interface
  // bump): an older host ignores it and answers the default form, which is
  // what every guest got before it existed.
  const bool resolve_links = input.resolveLinks.value_or(false);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Realpath, fs_host_, std::move(input.path),
                     resolve_links),
      base::BindOnce(&RespondRealpath, std::move(done)));
}

void NexusFsBridgeAdapter::HandleSystemInfoOnUI(
    ::nexus::generated::SystemInfoInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::SystemInfo, fs_host_),
      base::BindOnce(&RespondSystemInfo, std::move(done)));
}

void NexusFsBridgeAdapter::HandlePathsOnUI(
    ::nexus::generated::FsPathsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Paths, fs_host_),
      base::BindOnce(&RespondPaths, std::move(done)));
}

void NexusFsBridgeAdapter::HandleCopyFileOnUI(
    ::nexus::generated::FsCopyFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::CopyFile, fs_host_, std::move(input.src),
                     std::move(input.dst), input.overwrite.value_or(false)),
      base::BindOnce(&RespondSimple, std::move(done), "CopyFile failed"));
}

void NexusFsBridgeAdapter::HandleSymlinkOnUI(
    ::nexus::generated::FsSymlinkInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Symlink, fs_host_, std::move(input.target),
                     std::move(input.path)),
      base::BindOnce(&RespondSimple, std::move(done), "Symlink failed"));
}

void NexusFsBridgeAdapter::HandleReadlinkOnUI(
    ::nexus::generated::FsReadlinkInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Readlink, fs_host_, std::move(input.path)),
      base::BindOnce(&RespondReadlink, std::move(done)));
}

void NexusFsBridgeAdapter::HandleChmodOnUI(
    ::nexus::generated::FsChmodInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Chmod, fs_host_, std::move(input.path),
                     input.mode),
      base::BindOnce(&RespondChmod, std::move(done)));
}

// ===========================================================================
// fs.watch / fs.unwatch / the fs.onChange event
// ===========================================================================

void NexusFsBridgeAdapter::HandleWatchOnUI(
    ::nexus::generated::FsWatchInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.path.empty()) {
    std::move(done).InvalidArgs("fs.watch: path must be non-empty");
    return;
  }
  if (watch_ids_.size() >= kMaxWatches) {
    std::move(done).Error(
        "TOO_MANY_WATCHES",
        base::StringPrintf("fs.watch: at most %zu watches per connection",
                           kMaxWatches));
    return;
  }
  if (watch_core_.is_null()) {
    // One sequence for every watch of this connection: FilePathWatcher must
    // live and die on the sequence it was started on, and the poll walks the
    // disk (MayBlock). Events hop back here to UI, weak-guarded.
    watch_core_ = base::SequenceBound<NexusFsWatchCore>(
        base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
        base::BindPostTask(
            base::SequencedTaskRunner::GetCurrentDefault(),
            base::BindRepeating(&NexusFsBridgeAdapter::PushFsChangeEvent,
                                weak_factory_.GetWeakPtr())));
  }

  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.path);
  if (!path.IsAbsolute()) {
    path = base::FilePath::FromUTF8Unsafe(fs_host_->Cwd()).Append(path);
  }
  const std::string watch_id =
      base::StringPrintf("w%" PRIu64, ++next_watch_seq_);
  watch_ids_.insert(watch_id);
  watch_core_.AsyncCall(&NexusFsWatchCore::Add)
      .WithArgs(watch_id, path, input.recursive.value_or(false), force_poll_)
      .Then(base::BindOnce(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
             ::nexus::BridgeResponseCallback done,
             NexusFsWatchCore::AddResult result) {
            if (!result.ok) {
              if (self) {
                self->watch_ids_.erase(id);
              }
              std::move(done).Error(
                  "FS_ERROR",
                  result.error.empty() ? "fs.watch failed" : result.error);
              return;
            }
            Dict out;
            out.Set("watchId", id);
            out.Set("mode", result.mode);
            std::move(done).Success(std::move(out));
          },
          weak_factory_.GetWeakPtr(), watch_id, std::move(done)));
}

void NexusFsBridgeAdapter::HandleUnwatchOnUI(
    ::nexus::generated::FsUnwatchInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const bool found = watch_ids_.erase(input.watchId) > 0;
  if (found && !watch_core_.is_null()) {
    watch_core_.AsyncCall(base::IgnoreResult(&NexusFsWatchCore::Remove))
        .WithArgs(input.watchId);
  }
  Dict out;
  out.Set("found", found);
  std::move(done).Success(std::move(out));
}

void NexusFsBridgeAdapter::PushFsChangeEvent(FsWatchEvent event) {
  // A burst that raced an unwatch (or the connection closing) is dropped.
  if (!watch_ids_.contains(event.watch_id)) {
    return;
  }
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "fs");
  route.Set("action", "onChange");

  base::ListValue paths;
  for (std::string& changed : event.paths) {
    paths.Append(std::move(changed));
  }
  Dict payload;
  payload.Set("watchId", std::move(event.watch_id));
  payload.Set("paths", std::move(paths));
  payload.Set("kind", std::move(event.kind));
  if (event.overflow) {
    payload.Set("overflow", true);
  }
  SendEvent(route, Value(std::move(payload)));
}

void NexusFsBridgeAdapter::OnBridgeConnectionClosed() {
  // Destroying the core (on its own sequence) cancels every watch.
  watch_core_.Reset();
  watch_ids_.clear();
}

}  // namespace nexus
