#include "custom_browser/browser/nexus/tools/tool_base.h"

#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"

namespace nexus {

ToolBase::ToolBase(ToolSession* session, Profile* profile,
                   const std::string& session_key)
    : session_(session), profile_(profile), session_key_(session_key) {}

ToolBase::~ToolBase() = default;

scoped_refptr<base::SequencedTaskRunner> ToolBase::BlockingRunner() const {
  if (session_) {
    return session_->blocking_runner();
  }
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE});
}

void ToolBase::OnStop(OnStopReason /*reason*/) {
  // Default no-op. Subclasses that own per-session state override this
  // (e.g. BrowserTool::OnStop stops its actor::TaskId).
}

OnStopReason ParseOnStopReason(std::string_view reason,
                                OnStopReason fallback) {
  if (reason == "completed") return OnStopReason::kCompleted;
  if (reason == "user_stopped") return OnStopReason::kUserStopped;
  if (reason == "session_deleted") return OnStopReason::kSessionDeleted;
  if (reason == "disconnected") return OnStopReason::kDisconnected;
  if (reason == "shutdown") return OnStopReason::kShutdown;
  if (reason == "destructor") return OnStopReason::kDestructor;
  return fallback;
}

std::string_view OnStopReasonToString(OnStopReason reason) {
  switch (reason) {
    case OnStopReason::kCompleted: return "completed";
    case OnStopReason::kUserStopped: return "user_stopped";
    case OnStopReason::kSessionDeleted: return "session_deleted";
    case OnStopReason::kDisconnected: return "disconnected";
    case OnStopReason::kShutdown: return "shutdown";
    case OnStopReason::kDestructor: return "destructor";
  }
  return "unknown";
}

}  // namespace nexus
