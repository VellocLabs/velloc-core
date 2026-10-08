#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"

#include <utility>

#include "base/logging.h"

namespace nexus {

base::DictValue ServiceFailureParams(
    const std::optional<std::string>& service_code,
    const std::optional<std::string>& detail,
    const std::optional<std::string>& install_hint) {
  base::DictValue params;
  params.Set("detail", detail.value_or(std::string()));
  if (service_code.has_value() && !service_code->empty()) {
    params.Set("service_code", *service_code);
  }
  if (install_hint.has_value() && !install_hint->empty()) {
    params.Set("install_hint", *install_hint);
  }
  return params;
}

namespace {
// A callback that never resolved (destroyed, or overwritten by another).
// The runtime renders the sentence; `reason` says which of the two it was.
constexpr char kDroppedToolErrorCode[] = "tool_dropped";

base::DictValue DroppedParams(std::string_view reason) {
  base::DictValue params;
  params.Set("reason", reason);
  return params;
}
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
    DispatchError(kDroppedToolErrorCode, kDroppedToolErrorCode,
                  DroppedParams("discarded"));
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
    DispatchError(kDroppedToolErrorCode, kDroppedToolErrorCode,
                  DroppedParams("overwritten"));
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

void ToolCallback::Reject(std::string code, base::DictValue params) && {
  std::string message = code;
  DispatchError(std::move(code), std::move(message), std::move(params));
}

void ToolCallback::DispatchSuccess(base::Value value) {
  fired_ = true;
  if (on_success_) {
    std::move(on_success_).Run(std::move(value));
  }
  // Drop the error callback — we won't be using it.
  on_error_.Reset();
}

void ToolCallback::DispatchError(std::string code,
                                 std::string message,
                                 base::DictValue params) {
  fired_ = true;
  if (on_error_) {
    std::move(on_error_).Run(std::move(code), std::move(message),
                             std::move(params));
  }
  // Drop the success callback — we won't be using it.
  on_success_.Reset();
}

}  // namespace nexus
