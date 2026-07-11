#include "custom_browser/browser/nexus/api/fs/nexus_fs_bridge_adapter.h"

#include <stdint.h>

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/numerics/safe_conversions.h"
#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

}  // namespace

NexusFsBridgeAdapter::NexusFsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    const base::FilePath& cwd)
    : ::nexus::generated::FsApiBase(delegate),
      fs_host_(std::make_unique<NexusFsHost>(cwd)) {}

NexusFsBridgeAdapter::~NexusFsBridgeAdapter() = default;

// ===========================================================================
// Per-action thread_pool entry points
//
// Every method runs on base::ThreadPool with MayBlock() (the codegen-
// emitted dispatcher PostTasks the entry hop). Each does its blocking
// work via fs_host_ and fires `done` for the response. The
// BridgeResponseCallback marshals the actual SendXxxResponse to UI
// internally — these bodies have ZERO PostTasks. Inline-vs-port
// resolution for binary inputs is handled entirely by the codegen
// dispatcher; by the time we get here, `input.content` /
// `input.data` are already populated regardless of transport.
// ===========================================================================

void NexusFsBridgeAdapter::HandleCwdOnThreadPool(
    ::nexus::generated::CwdInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::move(done).Success(Value(fs_host_->Cwd()));
}

void NexusFsBridgeAdapter::HandleStatOnThreadPool(
    ::nexus::generated::StatInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::StatResult result = fs_host_->Stat(input.path);
  if (!result.success) {
    std::move(done).Error("FS_ERROR",
                          !result.error.empty() ? result.error : "Stat failed");
    return;
  }
  std::optional<Value> stat_value =
      base::JSONReader::Read(result.json, base::JSON_PARSE_RFC);
  if (!stat_value.has_value()) {
    std::move(done).Error("FS_ERROR", "Failed to decode stat response");
    return;
  }
  std::move(done).Success(std::move(*stat_value));
}

void NexusFsBridgeAdapter::HandleReaddirOnThreadPool(
    ::nexus::generated::ReaddirInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::ReaddirResult result = fs_host_->Readdir(input.path);
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Readdir failed");
    return;
  }
  std::optional<Value> dirents =
      base::JSONReader::Read(result.json, base::JSON_PARSE_RFC);
  if (!dirents.has_value()) {
    std::move(done).Error("FS_ERROR", "Failed to decode readdir response");
    return;
  }
  std::move(done).Success(std::move(*dirents));
}

void NexusFsBridgeAdapter::HandleReadFileOnThreadPool(
    ::nexus::generated::ReadFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::ReadResult result = fs_host_->ReadFile(input.path);
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "ReadFile failed");
    return;
  }
  std::move(done).Binary(
      Value(),
      result.data.has_value() ? std::move(*result.data) : mojo_base::BigBuffer(),
      "$result");
}

void NexusFsBridgeAdapter::HandleReadFileBytesOnThreadPool(
    ::nexus::generated::ReadFileBytesInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const int32_t max_bytes = std::max(0, input.maxBytes.value_or(0));
  if (max_bytes > 0) {
    NexusFsHost::ReadRangeResult range = fs_host_->ReadRange(
        input.path, /*offset=*/0, base::checked_cast<uint32_t>(max_bytes));
    if (!range.success) {
      std::move(done).Error(
          "FS_ERROR",
          !range.error.empty() ? range.error : "ReadFileBytes failed");
      return;
    }
    std::move(done).Binary(
        Value(),
        range.data.has_value() ? std::move(*range.data) : mojo_base::BigBuffer(),
        "$result");
    return;
  }

  NexusFsHost::ReadResult full = fs_host_->ReadFile(input.path);
  if (!full.success) {
    std::move(done).Error(
        "FS_ERROR",
        !full.error.empty() ? full.error : "ReadFileBytes failed");
    return;
  }
  std::move(done).Binary(
      Value(),
      full.data.has_value() ? std::move(*full.data) : mojo_base::BigBuffer(),
      "$result");
}

void NexusFsBridgeAdapter::HandleReadRangeOnThreadPool(
    ::nexus::generated::ReadRangeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const uint64_t offset = base::checked_cast<uint64_t>(
      std::max<int32_t>(0, input.offset.value_or(0)));
  const uint32_t length = base::checked_cast<uint32_t>(
      std::max<int32_t>(0, input.length.value_or(0)));

  NexusFsHost::ReadRangeResult result =
      fs_host_->ReadRange(input.path, offset, length);
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR",
        !result.error.empty() ? result.error : "ReadRange failed");
    return;
  }

  Dict envelope;
  envelope.Set("bytesRead", static_cast<int>(result.bytes_read));
  std::move(done).Binary(
      Value(std::move(envelope)),
      result.data.has_value() ? std::move(*result.data) : mojo_base::BigBuffer(),
      "data");
}

void NexusFsBridgeAdapter::HandleWriteFileOnThreadPool(
    ::nexus::generated::WriteFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // input.content is always populated here — the codegen dispatcher
  // resolved either inline-payload or transferred-port before invoking.
  NexusFsHost::SimpleResult result = fs_host_->WriteFile(
      input.path, mojo_base::BigBuffer(base::span(input.content)));
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "WriteFile failed");
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusFsBridgeAdapter::HandleAppendFileOnThreadPool(
    ::nexus::generated::AppendFileInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // input.data is always populated here (see writeFile).
  NexusFsHost::SimpleResult result = fs_host_->AppendFile(
      input.path, mojo_base::BigBuffer(base::span(input.data)));
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR",
        !result.error.empty() ? result.error : "AppendFile failed");
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusFsBridgeAdapter::HandleMkdirOnThreadPool(
    ::nexus::generated::MkdirInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::SimpleResult result = fs_host_->Mkdir(input.path);
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Mkdir failed");
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusFsBridgeAdapter::HandleRmOnThreadPool(
    ::nexus::generated::RmInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::SimpleResult result = fs_host_->Rm(
      input.path, input.recursive.value_or(false),
      input.force.value_or(false));
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Rm failed");
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusFsBridgeAdapter::HandleUnlinkOnThreadPool(
    ::nexus::generated::UnlinkInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::SimpleResult result = fs_host_->Unlink(input.path);
  if (!result.success) {
    std::move(done).Error(
        "FS_ERROR", !result.error.empty() ? result.error : "Unlink failed");
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusFsBridgeAdapter::HandleExistsOnThreadPool(
    ::nexus::generated::ExistsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusFsHost::ExistsResult result = fs_host_->Exists(input.path);
  if (!result.success && !result.error.empty()) {
    std::move(done).Error("FS_ERROR", result.error);
    return;
  }
  std::move(done).Success(Value(result.exists));
}

}  // namespace nexus
