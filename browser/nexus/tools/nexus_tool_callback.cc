#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/strcat.h"

namespace nexus {

namespace {
// Default error code emitted when a tool body fails without specifying
// one. Matches the convention used by other Nexus capability errors.
constexpr char kDefaultToolErrorCode[] = "tool_error";

// Error code used when a callback is destroyed without ever resolving.
// Surfaces a clear message rather than letting the guest's promise hang.
constexpr char kDroppedToolErrorCode[] = "tool_dropped";
}  // namespace

ToolCallback::ToolCallback() = default;

ToolCallback::ToolCallback(SuccessCallback on_success, ErrorCallback on_error)
    : on_success_(std::move(on_success)),
      on_error_(std::move(on_error)) {}

ToolCallback::~ToolCallback() {
  if (fired_) {
    return;
  }
  // The tool body received this callback but never resolved it. Emit a
  // synthetic error so the guest sees an explicit failure rather than a
  // hung promise. The bound `WeakPtr` inside `on_error_` will silently
  // drop the call if the adapter is also gone.
  if (on_error_) {
    LOG(WARNING) << "[NexusToolCallback] Callback destroyed without "
                    "resolving; emitting tool_dropped.";
    DispatchError(kDroppedToolErrorCode,
                  "tool dropped without resolving");
  }
}

ToolCallback::ToolCallback(ToolCallback&& other) noexcept
    : on_success_(std::move(other.on_success_)),
      on_error_(std::move(other.on_error_)),
      fired_(other.fired_) {
  // Mark the moved-from instance as fired so its destructor doesn't
  // emit a synthetic tool_dropped error.
  other.fired_ = true;
}

ToolCallback& ToolCallback::operator=(ToolCallback&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  // If `*this` had unfired state, drain it first — surface a dropped
  // error before we overwrite. (Matters only if a caller assigns onto an
  // in-flight callback, which would be a bug.)
  if (!fired_ && on_error_) {
    DispatchError(kDroppedToolErrorCode,
                  "tool dropped without resolving (overwritten)");
  }
  on_success_ = std::move(other.on_success_);
  on_error_ = std::move(other.on_error_);
  fired_ = other.fired_;
  other.fired_ = true;
  return *this;
}

void ToolCallback::Resolve(base::Value value) && {
  DispatchSuccess(std::move(value));
}

void ToolCallback::Resolve(base::DictValue value) && {
  DispatchSuccess(base::Value(std::move(value)));
}

void ToolCallback::Resolve(base::ListValue value) && {
  DispatchSuccess(base::Value(std::move(value)));
}

void ToolCallback::ResolveVoid() && {
  base::DictValue result;
  result.Set("ok", true);
  DispatchSuccess(base::Value(std::move(result)));
}

void ToolCallback::Reject(std::string message) && {
  DispatchError(kDefaultToolErrorCode, std::move(message));
}

void ToolCallback::Reject(std::string code, std::string message) && {
  DispatchError(std::move(code), std::move(message));
}

void ToolCallback::DispatchSuccess(base::Value value) {
  fired_ = true;
  if (on_success_) {
    std::move(on_success_).Run(std::move(value));
  }
  // Drop the error callback — we won't be using it.
  on_error_.Reset();
}

void ToolCallback::DispatchError(std::string code, std::string message) {
  fired_ = true;
  if (on_error_) {
    std::move(on_error_).Run(std::move(code), std::move(message));
  }
  // Drop the success callback — we won't be using it.
  on_success_.Reset();
}

namespace {

// Internal holder that owns the inner ToolCallback plus the timer.
// RefCounted so both the timer's OnTimeout method and the callback
// closures bound by BudgetWrap can hold it. First-write-wins is
// enforced by `inner_`'s own `fired_` flag — DispatchSuccess /
// DispatchError set `fired_ = true` and the second call becomes a
// no-op via the `on_*_.Reset()` paths.
class BudgetedHolder : public base::RefCounted<BudgetedHolder> {
 public:
  BudgetedHolder(ToolCallback inner,
                 base::TimeDelta budget,
                 std::string action_name)
      : inner_(std::move(inner)),
        action_name_(std::move(action_name)) {
    timer_.Start(FROM_HERE, budget, this, &BudgetedHolder::OnTimeout);
  }

  void Resolve(base::Value value) {
    timer_.Stop();
    if (inner_.fired()) {
      return;
    }
    std::move(inner_).Resolve(std::move(value));
  }

  void Reject(std::string code, std::string message) {
    timer_.Stop();
    if (inner_.fired()) {
      return;
    }
    std::move(inner_).Reject(std::move(code), std::move(message));
  }

 private:
  friend class base::RefCounted<BudgetedHolder>;
  ~BudgetedHolder() = default;

  void OnTimeout() {
    if (inner_.fired()) {
      return;
    }
    LOG(WARNING) << "[NexusToolCallback] " << action_name_
                 << " exceeded action_timeout";
    std::move(inner_).Reject(
        "tool_action_timeout",
        base::StrCat({action_name_,
                      ": exceeded per-action wall-clock budget"}));
  }

  ToolCallback inner_;
  std::string action_name_;
  base::OneShotTimer timer_;
};

}  // namespace

ToolCallback BudgetWrap(ToolCallback inner,
                        base::TimeDelta budget,
                        std::string action_name) {
  if (!budget.is_positive()) {
    return inner;
  }
  auto holder = base::MakeRefCounted<BudgetedHolder>(
      std::move(inner), budget, std::move(action_name));
  // Bind the holder into both bound callbacks so the holder lives
  // until BOTH paths drop their reference (timer fires or stops, and
  // a Resolve/Reject runs). first-write-wins is handled inside the
  // holder via inner_.fired().
  return ToolCallback(
      base::BindOnce(
          [](scoped_refptr<BudgetedHolder> h, base::Value v) {
            h->Resolve(std::move(v));
          },
          holder),
      base::BindOnce(
          [](scoped_refptr<BudgetedHolder> h, std::string c, std::string m) {
            h->Reject(std::move(c), std::move(m));
          },
          holder));
}

}  // namespace nexus
