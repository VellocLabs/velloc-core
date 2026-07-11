#include "custom_browser/browser/nexus/nexus_message_port_endpoint.h"

#include <memory>
#include <utility>

#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/connector.h"
#include "third_party/blink/public/common/messaging/message_port_channel.h"
#include "third_party/blink/public/common/messaging/transferable_message.h"
#include "third_party/blink/public/common/messaging/transferable_message_mojom_traits.h"
#include "third_party/blink/public/common/messaging/web_message_port.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/messaging/transferable_message.mojom.h"

namespace nexus {

NexusMessagePortEndpoint::NexusMessagePortEndpoint(
    blink::MessagePortDescriptor descriptor,
    MessageCallback on_message,
    ErrorCallback on_error)
    : descriptor_(std::move(descriptor)),
      on_message_(std::move(on_message)),
      on_error_(std::move(on_error)) {
  connector_ = std::make_unique<mojo::Connector>(
      descriptor_.TakeHandleToEntangleWithEmbedder(),
      mojo::Connector::SINGLE_THREADED_SEND);
  connector_->set_connection_error_handler(base::BindOnce(
      &NexusMessagePortEndpoint::HandlePipeError, base::Unretained(this)));

  if (on_message_) {
    connector_->set_incoming_receiver(this);
    connector_->StartReceiving(base::SingleThreadTaskRunner::GetCurrentDefault());
  }
}

NexusMessagePortEndpoint::~NexusMessagePortEndpoint() {
  Close();
}

bool NexusMessagePortEndpoint::Send(
    const blink::WebMessagePayload& payload,
    std::vector<blink::MessagePortDescriptor> ports) {
  if (!connector_ || connector_->encountered_error()) {
    return false;
  }

  blink::TransferableMessage transferable_message =
      blink::EncodeWebMessagePayload(payload);
  transferable_message.ports =
      blink::MessagePortChannel::CreateFromHandles(std::move(ports));
  transferable_message.sender_agent_cluster_id =
      blink::WebMessagePort::GetEmbedderAgentClusterID();
  transferable_message.task_state_id = std::nullopt;

  mojo::Message mojo_message =
      blink::mojom::TransferableMessage::SerializeAsMessage(
          &transferable_message);
  return connector_->Accept(&mojo_message);
}

bool NexusMessagePortEndpoint::IsConnected() const {
  return connector_ && !connector_->encountered_error();
}

void NexusMessagePortEndpoint::Close() {
  GiveDisentangledHandleIfNeeded();
}

bool NexusMessagePortEndpoint::Accept(mojo::Message* message) {
  blink::TransferableMessage transferable_message;
  if (!blink::mojom::TransferableMessage::DeserializeFromMessage(
          std::move(*message), &transferable_message)) {
    return false;
  }

  auto ports = std::move(transferable_message.ports);
  std::optional<blink::WebMessagePayload> payload =
      blink::DecodeToWebMessagePayload(std::move(transferable_message));
  if (!payload) {
    return true;
  }

  if (on_message_) {
    on_message_.Run(std::move(*payload),
                    blink::MessagePortChannel::ReleaseHandles(std::move(ports)));
  }
  return true;
}

void NexusMessagePortEndpoint::HandlePipeError() {
  connector_errored_ = true;
  if (on_error_) {
    on_error_.Run();
  }
}

void NexusMessagePortEndpoint::GiveDisentangledHandleIfNeeded() {
  if (!connector_ || !descriptor_.IsValid()) {
    return;
  }

  if (connector_errored_) {
    descriptor_.GiveDisentangledHandle(mojo::ScopedMessagePipeHandle());
  } else {
    descriptor_.GiveDisentangledHandle(connector_->PassMessagePipe());
  }
  connector_.reset();
}

}  // namespace nexus
