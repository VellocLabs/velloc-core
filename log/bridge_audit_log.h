#ifndef CUSTOM_BROWSER_LOG_BRIDGE_AUDIT_LOG_H_
#define CUSTOM_BROWSER_LOG_BRIDGE_AUDIT_LOG_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/common/chrome_paths.h"
#include "custom_browser/log/agent_log.h"

// The bridge audit log: one `key=value|...` line per bridge request, written by
// NexusBridgePortService on response; the runtime cannot suppress it. A line
// holds id, route, session key (attribution, never authorization), an argument
// digest (never file contents, command text, secrets, headers or env), outcome
// and duration. File: <user-data-dir>/custom_browser/log/bridge_audit.log,
// rotated at kBridgeAuditMaxBytes keeping kBridgeAuditKeepFiles older files;
// appends run on a MayBlock worker.

namespace custom_browser::log {

inline constexpr int64_t kBridgeAuditMaxBytes = 8 * 1024 * 1024;
inline constexpr int kBridgeAuditKeepFiles = 8;

inline base::FilePath GetBridgeAuditLogPath() {
  base::FilePath user_data_dir;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir) &&
      !user_data_dir.empty()) {
    return user_data_dir.AppendASCII("custom_browser")
        .AppendASCII("log")
        .AppendASCII("bridge_audit.log");
  }
  base::FilePath temp_dir;
  if (base::PathService::Get(base::DIR_TEMP, &temp_dir) &&
      !temp_dir.empty()) {
    return temp_dir.AppendASCII("custom_browser_bridge_audit.log");
  }
  return base::FilePath(FILE_PATH_LITERAL("bridge_audit.log"));
}

// Shifts bridge_audit.log.N -> .N+1 (dropping the oldest) and the live file
// to .1 once it reaches the size cap.
inline void RotateBridgeAuditLogIfNeeded(const base::FilePath& log_file) {
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

inline void AppendBridgeAuditLineOnWorker(std::string line) {
  const base::FilePath log_file = GetBridgeAuditLogPath();
  if (!base::CreateDirectory(log_file.DirName())) {
    LOG(INFO) << "[BRIDGE_AUDIT_FILE_ERROR] create_dir_failed path="
              << log_file.DirName().AsUTF8Unsafe();
    return;
  }
  RotateBridgeAuditLogIfNeeded(log_file);
  line.push_back('\n');
  if (!base::PathExists(log_file) &&
      !base::WriteFile(log_file, std::string_view())) {
    LOG(INFO) << "[BRIDGE_AUDIT_FILE_ERROR] create_failed path="
              << log_file.AsUTF8Unsafe();
    return;
  }
  if (!base::AppendToFile(log_file, line)) {
    LOG(INFO) << "[BRIDGE_AUDIT_FILE_ERROR] append_failed path="
              << log_file.AsUTF8Unsafe();
  }
}

inline void AppendBridgeAuditLine(std::string line) {
  if (!base::ThreadPoolInstance::Get()) {
    AppendBridgeAuditLineOnWorker(std::move(line));
    return;
  }
  if (!base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
          base::BindOnce(&AppendBridgeAuditLineOnWorker, std::move(line)))) {
    AppendBridgeAuditLineOnWorker(std::move(line));
  }
}

// Builder for one audit line. Values are sanitized like the agent log's
// (newlines, tabs and the separator replaced).
class BridgeAuditLine {
 public:
  BridgeAuditLine() {
    line_ = base::StrCat(
        {"ts_ms=",
         base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch())});
  }

  BridgeAuditLine& Add(std::string_view key, std::string_view value) {
    line_.append("|");
    line_.append(SanitizeAgentLogValue(key));
    line_.append("=");
    line_.append(SanitizeAgentLogValue(value));
    return *this;
  }

  BridgeAuditLine& Add(std::string_view key, int64_t value) {
    return Add(key, base::NumberToString(value));
  }

  const std::string& line() const { return line_; }

  void Commit() && { AppendBridgeAuditLine(std::move(line_)); }

 private:
  std::string line_;
};

}  // namespace custom_browser::log

#endif  // CUSTOM_BROWSER_LOG_BRIDGE_AUDIT_LOG_H_
