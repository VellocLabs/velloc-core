#include "custom_browser/browser/nexus/tools/tool_session.h"

#include <utility>

#include "base/logging.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"

namespace nexus {

ToolSession::ToolSession(Profile* profile, std::string session_key)
    : profile_(profile),
      session_key_(std::move(session_key)),
      // `this` is well-formed at this point — the storage is allocated
      // and the vptrs / non-static-member init has finished. Each
      // category stores the back-pointer for later session-level reads
      // (cwd, action_timeout, …); none of them dereference `this`
      // during their own ctor body.
      browser_tool_(this, profile, session_key_),
      file_tool_(this, profile, session_key_),
      process_tool_(this, profile, session_key_) {
  // Future categories construct here too:
  //   skill_tool_(this, profile, session_key_)
}

ToolSession::~ToolSession() {
  // Defensive cascade — if the session is destructed without an
  // explicit OnStop having run first (e.g. profile shutdown bypasses
  // the bridge layer), each category still gets a final notification.
  OnStop(OnStopReason::kDestructor);
}

void ToolSession::OnStop(OnStopReason reason) {
  LOG(INFO) << "[ToolSession] OnStop session=" << session_key_
            << " reason=" << OnStopReasonToString(reason);
  ClearProgressSink();
  browser_tool_.OnStop(reason);
  file_tool_.OnStop(reason);
  process_tool_.OnStop(reason);
  // skill_tool_.OnStop(reason);  // future
}

scoped_refptr<base::SequencedTaskRunner> ToolSession::blocking_runner() {
  if (!blocking_runner_) {
    blocking_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE});
  }
  return blocking_runner_;
}

void ToolSession::set_progress_sink(ProgressSink sink) {
  progress_sink_ = std::move(sink);
}

void ToolSession::ClearProgressSink() {
  progress_sink_.Reset();
}

void ToolSession::EmitToolProgress(base::Value progress) {
  if (progress_sink_) {
    progress_sink_.Run(std::move(progress));
  }
}

void ToolSession::set_action_timeout(base::TimeDelta timeout) {
  // Clamp to a sane window so a misconfigured caller can't disable
  // the budget (zero/negative) or set an unreasonably-long one. Range
  // matches the JS-side validation contract:
  // 1s .. 600s.
  constexpr base::TimeDelta kMin = base::Seconds(1);
  constexpr base::TimeDelta kMax = base::Seconds(600);
  if (timeout < kMin) {
    action_timeout_ = kMin;
  } else if (timeout > kMax) {
    action_timeout_ = kMax;
  } else {
    action_timeout_ = timeout;
  }
  // Propagate to per-category members so each tool method's actor
  // wrapper sees the freshly-set value without the category needing
  // to keep a back-pointer to ToolSession.
  browser_tool_.set_action_timeout(action_timeout_);
}

void ToolSession::set_step_timeout(base::TimeDelta timeout) {
  constexpr base::TimeDelta kMin = base::Seconds(1);
  constexpr base::TimeDelta kMax = base::Seconds(600);
  if (timeout < kMin) {
    step_timeout_ = kMin;
  } else if (timeout > kMax) {
    step_timeout_ = kMax;
  } else {
    step_timeout_ = timeout;
  }
  browser_tool_.set_step_timeout(step_timeout_);
}

void ToolSession::set_cwd(base::FilePath cwd) {
  // Reject relative paths early — accepting one and letting downstream
  // tools resolve it against the process cwd would be a stealth misfire
  // (the agent's intended cwd is almost never the browser process's
  // launch directory). An empty path also short-circuits here so a
  // caller can clear the session cwd by passing `base::FilePath()`.
  if (!cwd.empty() && !cwd.IsAbsolute()) {
    LOG(WARNING) << "[ToolSession] set_cwd ignored non-absolute path: "
                 << cwd.AsUTF8Unsafe();
    return;
  }
  cwd_ = std::move(cwd);
  LOG(INFO) << "[ToolSession] set_cwd session=" << session_key_
            << " cwd=" << cwd_.AsUTF8Unsafe();
}

}  // namespace nexus
