#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_message_port_endpoint.h"

namespace nexus {

// ---- BinaryReceiveResult --------------------------------------------------

BinaryReceiveResult::BinaryReceiveResult() = default;
BinaryReceiveResult::BinaryReceiveResult(BinaryReceiveResult&&) noexcept =
    default;
BinaryReceiveResult& BinaryReceiveResult::operator=(
    BinaryReceiveResult&&) noexcept = default;
BinaryReceiveResult::~BinaryReceiveResult() = default;

// ---- PendingBinaryReceive -------------------------------------------------

NexusBridgeCapabilityHandler::PendingBinaryReceive::PendingBinaryReceive() =
    default;
NexusBridgeCapabilityHandler::PendingBinaryReceive::PendingBinaryReceive(
    PendingBinaryReceive&&) noexcept = default;
NexusBridgeCapabilityHandler::PendingBinaryReceive&
NexusBridgeCapabilityHandler::PendingBinaryReceive::operator=(
    PendingBinaryReceive&&) noexcept = default;
NexusBridgeCapabilityHandler::PendingBinaryReceive::~PendingBinaryReceive() =
    default;

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

void NexusBridgeCapabilityHandler::SendEvent(const base::DictValue& route,
                                             base::Value payload) const {
  delegate_->SendEvent(route, std::move(payload));
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

// ---- AwaitBinaryFromPort --------------------------------------------------

void NexusBridgeCapabilityHandler::AwaitBinaryFromPort(
    blink::MessagePortDescriptor port,
    base::OnceCallback<void(BinaryReceiveResult)> on_done) {
  // Mint a unique receive id and stash the pending entry. Both the
  // OnMessage and OnError closures look up the entry by id; the first
  // to find a non-empty `on_done` consumes it and erases the entry.
  const int receive_id = next_binary_receive_id_++;

  PendingBinaryReceive pending;
  pending.on_done = std::move(on_done);
  pending.endpoint = std::make_unique<NexusMessagePortEndpoint>(
      std::move(port),
      base::BindRepeating(
          [](base::WeakPtr<NexusBridgeCapabilityHandler> self, int receive_id,
             blink::WebMessagePayload payload,
             std::vector<blink::MessagePortDescriptor> nested_ports) {
            if (!self) {
              return;
            }
            if (!nested_ports.empty()) {
              LOG(WARNING)
                  << "[NexusBridgePort] Ignoring nested transferred ports for "
                  << "binary receive " << receive_id;
            }
            BinaryReceiveResult result;
            if (!bridge_support::IsBinaryPayload(payload)) {
              result.ok = false;
              result.error =
                  "Binary data port payload must be an ArrayBuffer";
            } else {
              result.ok = true;
              result.bytes = bridge_support::PayloadToBytes(payload);
            }
            self->FinishPendingBinaryReceive(receive_id, std::move(result));
          },
          weak_factory_.GetWeakPtr(), receive_id),
      base::BindRepeating(
          [](base::WeakPtr<NexusBridgeCapabilityHandler> self,
             int receive_id) {
            if (!self) {
              return;
            }
            // Defer one tick so any in-flight OnMessage drains first.
            // If OnMessage already fired, the entry is gone and
            // FinishPendingBinaryReceive is a no-op.
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE,
                base::BindOnce(
                    [](base::WeakPtr<NexusBridgeCapabilityHandler> self,
                       int receive_id) {
                      if (!self) {
                        return;
                      }
                      BinaryReceiveResult result;
                      result.ok = false;
                      result.error = "Binary data port disconnected";
                      self->FinishPendingBinaryReceive(receive_id,
                                                       std::move(result));
                    },
                    self, receive_id));
          },
          weak_factory_.GetWeakPtr(), receive_id));

  pending_binary_receives_[receive_id] = std::move(pending);
}

void NexusBridgeCapabilityHandler::FinishPendingBinaryReceive(
    int receive_id,
    BinaryReceiveResult result) {
  auto it = pending_binary_receives_.find(receive_id);
  if (it == pending_binary_receives_.end()) {
    return;  // Already settled by the other path.
  }
  if (!it->second.on_done) {
    return;
  }
  base::OnceCallback<void(BinaryReceiveResult)> callback =
      std::move(it->second.on_done);
  pending_binary_receives_.erase(it);
  std::move(callback).Run(std::move(result));
}

}  // namespace nexus
