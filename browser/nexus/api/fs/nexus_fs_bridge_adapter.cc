#include "custom_browser/browser/nexus/api/fs/nexus_fs_bridge_adapter.h"

#include <stdint.h>

#include <algorithm>
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
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

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

void RespondSimple(BridgeResponseCallback done,
                   const char* error_label,
                   NexusFsHost::SimpleResult result) {
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : error_label);
    return;
  }
  std::move(done).SuccessVoid();
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
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](scoped_refptr<NexusFsHost> host, std::string path,
             std::vector<uint8_t> content) {
            return host->WriteFile(path,
                                   mojo_base::BigBuffer(base::span(content)));
          },
          fs_host_, std::move(input.path), std::move(input.content)),
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
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NexusFsHost::Realpath, fs_host_, std::move(input.path)),
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

}  // namespace nexus
