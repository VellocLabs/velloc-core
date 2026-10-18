#include "custom_browser/log/bridge_audit_log.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/platform_thread.h"
#include "base/threading/scoped_blocking_call.h"

namespace custom_browser::log {

void RotateBridgeAuditLogIfNeeded(const base::FilePath& log_file) {
  const std::optional<int64_t> file_size = base::GetFileSize(log_file);
  if (!file_size || *file_size < kBridgeAuditMaxBytes) {
    return;
  }
  const base::FilePath oldest =
      log_file.AddExtensionASCII(base::NumberToString(kBridgeAuditKeepFiles));
  if (base::PathExists(oldest) && !base::DeleteFile(oldest)) {
    return;
  }
  for (int n = kBridgeAuditKeepFiles - 1; n >= 1; --n) {
    const base::FilePath from =
        log_file.AddExtensionASCII(base::NumberToString(n));
    const base::FilePath to =
        log_file.AddExtensionASCII(base::NumberToString(n + 1));
    if (base::PathExists(from)) {
      base::Move(from, to);
    }
  }
  base::Move(log_file, log_file.AddExtensionASCII("1"));
}

bool WriteBridgeAuditData(const base::FilePath& log_file,
                          std::string_view data) {
  if (!base::CreateDirectory(log_file.DirName())) {
    LOG(INFO) << "[BRIDGE_AUDIT_FILE_ERROR] create_dir_failed path="
              << log_file.DirName().AsUTF8Unsafe();
    return false;
  }
  RotateBridgeAuditLogIfNeeded(log_file);
  for (int attempt = 1;; ++attempt) {
    base::File file(log_file,
                    base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
    if (file.IsValid()) {
      if (file.WriteAtCurrentPosAndCheck(base::as_byte_span(data))) {
        return true;
      }
      LOG(INFO) << "[BRIDGE_AUDIT_FILE_ERROR] append_failed path="
                << log_file.AsUTF8Unsafe();
      return false;
    }
    const base::File::Error error = file.error_details();
    const bool retryable = error == base::File::FILE_ERROR_IN_USE ||
                           error == base::File::FILE_ERROR_ACCESS_DENIED;
    if (!retryable || attempt >= kBridgeAuditOpenAttempts) {
      LOG(INFO) << "[BRIDGE_AUDIT_FILE_ERROR] open_failed path="
                << log_file.AsUTF8Unsafe()
                << " error=" << base::File::ErrorToString(error)
                << " attempts=" << attempt;
      return false;
    }
    base::ScopedBlockingCall scoped_blocking_call(
        FROM_HERE, base::BlockingType::MAY_BLOCK);
    base::PlatformThread::Sleep(kBridgeAuditOpenRetryDelay);
  }
}

// static
BridgeAuditWriter& BridgeAuditWriter::Get() {
  static base::NoDestructor<BridgeAuditWriter> writer;
  return *writer;
}

BridgeAuditWriter::BridgeAuditWriter() = default;

BridgeAuditWriter::~BridgeAuditWriter() = default;

void BridgeAuditWriter::Append(std::string line) {
  line.push_back('\n');
  {
    base::AutoLock lock(lock_);
    pending_.push_back(std::move(line));
    if (drain_scheduled_) {
      return;
    }
    drain_scheduled_ = true;
  }
  // A queued drain blocks shutdown, so a drain posted before shutdown completes
  // always runs (posting after it completes is a caller ordering bug the pool
  // DCHECKs). A post the pool refuses drains here, still holding scheduling.
  if (base::ThreadPoolInstance::Get() &&
      base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
          base::BindOnce(&BridgeAuditWriter::Drain, base::Unretained(this)))) {
    return;
  }
  Drain();
}

void BridgeAuditWriter::SetPathForTesting(base::FilePath path) {
  base::AutoLock lock(lock_);
  path_for_testing_ = std::move(path);
}

void BridgeAuditWriter::Drain() {
  for (;;) {
    std::string data;
    base::FilePath log_file;
    {
      base::AutoLock lock(lock_);
      if (pending_.empty()) {
        drain_scheduled_ = false;
        return;
      }
      for (const std::string& line : pending_) {
        data.append(line);
      }
      pending_.clear();
      log_file = path_for_testing_;
    }
    if (log_file.empty()) {
      log_file = GetBridgeAuditLogPath();
    }
    WriteBridgeAuditData(log_file, data);
  }
}

}  // namespace custom_browser::log
