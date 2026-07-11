#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_SESSION_TYPES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_SESSION_TYPES_H_

#include <string>

#include "base/time/time.h"
#include "url/origin.h"

namespace nexus {

// Lifecycle state of an agent task session.
enum class SessionStatus {
  kActive,
  kCompleted,
  kInterrupted,
  kPaused,
};

// Persistent metadata for one agent interaction session within a domain
// sandbox. The webapp manages actual conversation content in its own IndexedDB;
// the browser only tracks this lightweight index for the session list UI and
// lifecycle management.
struct SessionMetadata {
  std::string session_id;
  std::string title;
  SessionStatus status = SessionStatus::kActive;
  base::Time created_at;
  base::Time last_activity;
  int message_count = 0;
  url::Origin domain_origin;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_SESSION_TYPES_H_
