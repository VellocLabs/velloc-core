#ifndef CUSTOM_BROWSER_LOG_AGENT_LOG_H_
#define CUSTOM_BROWSER_LOG_AGENT_LOG_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/time/time.h"
#include "chrome/common/chrome_paths.h"

namespace custom_browser::log {

inline constexpr int64_t kAgentLogMaxBytes = 512 * 1024;

inline std::string SanitizeAgentLogValue(std::string_view value) {
  std::string sanitized(value);
  base::ReplaceChars(sanitized, "\r\n\t|", "____", &sanitized);
  return sanitized;
}

inline base::FilePath GetAgentLogPath() {
  base::FilePath user_data_dir;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir) &&
      !user_data_dir.empty()) {
    return user_data_dir.AppendASCII("custom_browser")
        .AppendASCII("log")
        .AppendASCII("agent_log.log");
  }

  base::FilePath temp_dir;
  if (base::PathService::Get(base::DIR_TEMP, &temp_dir) &&
      !temp_dir.empty()) {
    return temp_dir.AppendASCII("custom_browser_agent_log.log");
  }
  return base::FilePath(FILE_PATH_LITERAL("agent_log.log"));
}

inline void RotateAgentLogFileIfNeeded(const base::FilePath& log_file) {
  const std::optional<int64_t> file_size = base::GetFileSize(log_file);
  if (!file_size || *file_size < kAgentLogMaxBytes) {
    return;
  }
  const base::FilePath rotated_file = log_file.AddExtensionASCII("1");
  if (base::PathExists(rotated_file) && !base::DeleteFile(rotated_file)) {
    return;
  }
  if (!base::Move(log_file, rotated_file)) {
    return;
  }
}

inline void AppendAgentLogLineOnWorker(std::string line) {
  const base::FilePath log_file = GetAgentLogPath();
  if (!base::CreateDirectory(log_file.DirName())) {
    LOG(INFO) << "[AGENT_LOG_FILE_ERROR] create_dir_failed path="
              << log_file.DirName().AsUTF8Unsafe();
    return;
  }
  RotateAgentLogFileIfNeeded(log_file);

  line.push_back('\n');
  if (!base::PathExists(log_file)) {
    if (!base::WriteFile(log_file, std::string_view())) {
      LOG(INFO) << "[AGENT_LOG_FILE_ERROR] create_failed path="
                << log_file.AsUTF8Unsafe();
      return;
    }
  }
  if (!base::AppendToFile(log_file, line)) {
    LOG(INFO) << "[AGENT_LOG_FILE_ERROR] append_failed path="
              << log_file.AsUTF8Unsafe();
    return;
  }
}

inline void AppendAgentLogLine(std::string line) {
  if (!base::ThreadPoolInstance::Get()) {
    LOG(INFO) << "[AGENT_LOG_FILE_WARN] thread_pool_missing";
    AppendAgentLogLineOnWorker(std::move(line));
    return;
  }
  if (!base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&AppendAgentLogLineOnWorker, std::move(line)))) {
    LOG(INFO) << "[AGENT_LOG_FILE_WARN] post_task_rejected";
    AppendAgentLogLineOnWorker(std::move(line));
  }
}

class AgentLog {
 public:
  AgentLog(std::string_view feature, std::string_view event) {
    line_ = base::StrCat(
        {"ts_ms=",
         base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch()),
         "|feature=", SanitizeAgentLogValue(feature),
         "|event=", SanitizeAgentLogValue(event)});
  }

  AgentLog& AddBool(std::string_view key, bool value) {
    return AddString(key, value ? "1" : "0");
  }

  AgentLog& AddInt(std::string_view key, int64_t value) {
    return AddString(key, base::NumberToString(value));
  }

  AgentLog& AddString(std::string_view key, std::string_view value) {
    line_.append("|");
    line_.append(SanitizeAgentLogValue(key));
    line_.append("=");
    line_.append(SanitizeAgentLogValue(value));
    return *this;
  }

  void Commit() const {
    LOG(INFO) << "[AGENT_LOG] " << line_;
    AppendAgentLogLine(line_);
  }

 private:
  std::string line_;
};

}  // namespace custom_browser::log

#endif  // CUSTOM_BROWSER_LOG_AGENT_LOG_H_
