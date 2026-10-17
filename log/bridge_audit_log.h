#ifndef CUSTOM_BROWSER_LOG_BRIDGE_AUDIT_LOG_H_
#define CUSTOM_BROWSER_LOG_BRIDGE_AUDIT_LOG_H_

#include <stdint.h>

#include <deque>
#include <string>
#include <string_view>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "chrome/common/chrome_paths.h"
#include "custom_browser/log/agent_log.h"

// The bridge audit log: one `key=value|...` line per bridge request, written by
// NexusBridgePortService on response; the runtime cannot suppress it. A line
// holds id, route, session key (attribution, never authorization), an argument
// digest (never file contents, command text, secrets, headers or env), outcome
// and duration. File: <user-data-dir>/custom_browser/log/bridge_audit.log,
// rotated at kBridgeAuditMaxBytes keeping kBridgeAuditKeepFiles older files.
// Appends are queued and written by one MayBlock drain at a time, so writes
// never overlap and lines keep their commit order.

namespace custom_browser::log {

inline constexpr int64_t kBridgeAuditMaxBytes = 8 * 1024 * 1024;
inline constexpr int kBridgeAuditKeepFiles = 8;
// An open that fails because another process holds the file without write
// sharing is retried this many times, kBridgeAuditOpenRetryDelay apart.
inline constexpr int kBridgeAuditOpenAttempts = 20;
inline constexpr base::TimeDelta kBridgeAuditOpenRetryDelay =
    base::Milliseconds(25);

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
void RotateBridgeAuditLogIfNeeded(const base::FilePath& log_file);

// Appends `data` (one or more newline-terminated lines) to `log_file`,
// creating it and its directory as needed. The file is opened with read and
// write sharing, so a reader holding it open does not block the append; an
// open refused by an exclusive holder is retried kBridgeAuditOpenAttempts
// times.
// Blocking; returns false (after logging) if the data was not written.
bool WriteBridgeAuditData(const base::FilePath& log_file,
                          std::string_view data);

// Process-wide append queue. Thread-safe; at most one drain runs at a time.
class BridgeAuditWriter {
 public:
  static BridgeAuditWriter& Get();

  BridgeAuditWriter();
  BridgeAuditWriter(const BridgeAuditWriter&) = delete;
  BridgeAuditWriter& operator=(const BridgeAuditWriter&) = delete;
  ~BridgeAuditWriter();

  void Append(std::string line);

  // Redirects writes to `path`; empty restores GetBridgeAuditLogPath().
  void SetPathForTesting(base::FilePath path);

 private:
  void Drain(base::ScopedClosureRunner on_dropped);
  void OnDrainDropped();

  base::Lock lock_;
  std::deque<std::string> pending_ GUARDED_BY(lock_);
  bool drain_scheduled_ GUARDED_BY(lock_) = false;
  base::FilePath path_for_testing_ GUARDED_BY(lock_);
};

inline void AppendBridgeAuditLine(std::string line) {
  BridgeAuditWriter::Get().Append(std::move(line));
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
