#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_MESSAGE_PORT_ENDPOINT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_MESSAGE_PORT_ENDPOINT_H_

#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/message.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "third_party/blink/public/common/messaging/string_message_codec.h"

namespace mojo {
class Connector;
}

namespace nexus {

class NexusMessagePortEndpoint : public mojo::MessageReceiver {
 public:
  using MessageCallback = base::RepeatingCallback<void(
      blink::WebMessagePayload payload,
      std::vector<blink::MessagePortDescriptor> ports)>;
  using ErrorCallback = base::RepeatingClosure;

  NexusMessagePortEndpoint(blink::MessagePortDescriptor descriptor,
                           MessageCallback on_message,
                           ErrorCallback on_error);
  ~NexusMessagePortEndpoint() override;

  NexusMessagePortEndpoint(const NexusMessagePortEndpoint&) = delete;
  NexusMessagePortEndpoint& operator=(const NexusMessagePortEndpoint&) = delete;

  bool Send(const blink::WebMessagePayload& payload,
            std::vector<blink::MessagePortDescriptor> ports = {});
  bool IsConnected() const;
  void Close();

 private:
  bool Accept(mojo::Message* message) override;
  void HandlePipeError();
  void GiveDisentangledHandleIfNeeded();

  blink::MessagePortDescriptor descriptor_;
  MessageCallback on_message_;
  ErrorCallback on_error_;
  std::unique_ptr<mojo::Connector> connector_;
  bool connector_errored_ = false;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_MESSAGE_PORT_ENDPOINT_H_
