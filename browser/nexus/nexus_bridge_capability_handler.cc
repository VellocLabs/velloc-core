#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"

#include <utility>

#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"

namespace nexus {

NexusBridgeCapabilityHandler::NexusBridgeCapabilityHandler(
    NexusBridgeResponseDelegate* delegate)
    : delegate_(delegate) {}

NexusBridgeCapabilityHandler::~NexusBridgeCapabilityHandler() = default;

bool NexusBridgeCapabilityHandler::Dispatch(
    std::string_view action,
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& payload,
    std::vector<blink::MessagePortDescriptor> ports) {
  auto it = handlers_.find(action);
  if (it == handlers_.end()) {
    return false;
  }

  it->second(id, route, payload, std::move(ports));
  return true;
}

void NexusBridgeCapabilityHandler::SendSuccessResponse(
    const std::string& id,
    const base::DictValue& route,
    base::Value result) const {
  delegate_->SendSuccessResponse(id, route, std::move(result));
}

void NexusBridgeCapabilityHandler::SendBinaryResponse(
    const std::string& id,
    const base::DictValue& route,
    base::Value result,
    mojo_base::BigBuffer data,
    std::string_view result_binary_field) const {
  delegate_->SendBinaryResponse(id, route, std::move(result), std::move(data),
                                result_binary_field);
}

void NexusBridgeCapabilityHandler::SendErrorResponse(
    const std::string& id,
    const base::DictValue& route,
    const std::string& code,
    const std::string& message) const {
  delegate_->SendErrorResponse(id, route, code, message);
}

void NexusBridgeCapabilityHandler::SendInvalidArgsResponse(
    const std::string& id,
    const base::DictValue& route,
    std::string_view message) const {
  SendErrorResponse(id, route, "INVALID_ARGS", std::string(message));
}

bool NexusBridgeCapabilityHandler::CopyRequiredStringOrSendInvalidArgs(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& payload,
    std::string_view key,
    std::string* out) const {
  std::string error_message;
  if (bridge_support::CopyRequiredString(payload, key, out, &error_message)) {
    return true;
  }

  SendInvalidArgsResponse(id, route, error_message);
  return false;
}

void NexusBridgeCapabilityHandler::RegisterHandler(std::string_view action,
                                                  DispatchHandler handler) {
  handlers_.insert_or_assign(std::string(action), std::move(handler));
}

}  // namespace nexus
