#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_CAPABILITY_HANDLER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_CAPABILITY_HANDLER_H_

#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

namespace nexus {

class NexusBridgeResponseDelegate;

class NexusBridgeCapabilityHandler {
 public:
  explicit NexusBridgeCapabilityHandler(NexusBridgeResponseDelegate* delegate);
  virtual ~NexusBridgeCapabilityHandler();

  NexusBridgeCapabilityHandler(const NexusBridgeCapabilityHandler&) = delete;
  NexusBridgeCapabilityHandler& operator=(const NexusBridgeCapabilityHandler&) =
      delete;

  bool Dispatch(std::string_view action,
                const std::string& id,
                const base::DictValue& route,
                const base::DictValue& payload,
                std::vector<blink::MessagePortDescriptor> ports);

 protected:
  template <typename Derived>
  void Register(std::string_view action,
                void (Derived::*handler)(
                    const std::string& id,
                    const base::DictValue& route,
                    const base::DictValue& payload,
                    std::vector<blink::MessagePortDescriptor> ports));

  template <typename Derived>
  void Register(std::string_view action,
                std::string_view required_key,
                void (Derived::*handler)(
                    const std::string& id,
                    const base::DictValue& route,
                    std::string required_arg,
                    std::vector<blink::MessagePortDescriptor> ports));

  template <typename Derived>
  void Register(std::string_view action,
                std::string_view required_key,
                void (Derived::*handler)(
                    const std::string& id,
                    const base::DictValue& route,
                    std::string required_arg,
                    const base::DictValue& payload,
                    std::vector<blink::MessagePortDescriptor> ports));

  template <typename Derived>
  void Register(std::string_view action,
                std::string_view first_required_key,
                std::string_view second_required_key,
                void (Derived::*handler)(
                    const std::string& id,
                    const base::DictValue& route,
                    std::string first_required_arg,
                    std::string second_required_arg,
                    std::vector<blink::MessagePortDescriptor> ports));

  template <typename Derived>
  void Register(std::string_view action,
                std::string_view first_required_key,
                std::string_view second_required_key,
                void (Derived::*handler)(
                    const std::string& id,
                    const base::DictValue& route,
                    std::string first_required_arg,
                    std::string second_required_arg,
                    const base::DictValue& payload,
                    std::vector<blink::MessagePortDescriptor> ports));

  void SendSuccessResponse(const std::string& id,
                           const base::DictValue& route,
                           base::Value result) const;
  void SendBinaryResponse(const std::string& id,
                          const base::DictValue& route,
                          base::Value result,
                          mojo_base::BigBuffer data,
                          std::string_view result_binary_field) const;
  void SendErrorResponse(const std::string& id,
                         const base::DictValue& route,
                         const std::string& code,
                         const std::string& message) const;
  void SendInvalidArgsResponse(const std::string& id,
                               const base::DictValue& route,
                               std::string_view message) const;
  // Host-pushed event (no client request to correlate with). Subclasses
  // use this to deliver server-initiated notifications to the guest.
  void SendEvent(const base::DictValue& route, base::Value payload) const;
  bool CopyRequiredStringOrSendInvalidArgs(const std::string& id,
                                           const base::DictValue& route,
                                           const base::DictValue& payload,
                                           std::string_view key,
                                           std::string* out) const;

  // Type alias for the shape that goes into the dispatch table. Exposed
  // protected so the macro-DSL adapter (`ToolsAdapter`) can synthesize
  // its own session-routing dispatcher lambdas via `RegisterHandler`
  // below without duplicating the parent class's dispatch machinery.
  using DispatchHandler = std::function<void(
      const std::string& id,
      const base::DictValue& route,
      const base::DictValue& payload,
      std::vector<blink::MessagePortDescriptor> ports)>;

  // Registers an arbitrary callable under `action`. The existing typed
  // `Register(...)` overloads above are sugar over this primitive; the
  // tool framework uses it directly to bind the macro-generated
  // dispatcher closures (which decode args + manufacture a
  // `ToolCallback` per request and forward to a `ToolDispatcher`).
  void RegisterHandler(std::string_view action, DispatchHandler handler);

 private:
  template <size_t N>
  bool CopyRequiredStringsOrSendInvalidArgs(
      const std::string& id,
      const base::DictValue& route,
      const base::DictValue& payload,
      const std::array<std::string_view, N>& required_keys,
      std::array<std::string, N>* out) const;

  raw_ptr<NexusBridgeResponseDelegate> delegate_;
  std::map<std::string, DispatchHandler, std::less<>> handlers_;
};

template <typename Derived>
void NexusBridgeCapabilityHandler::Register(
    std::string_view action,
    void (Derived::*handler)(const std::string&,
                             const base::DictValue&,
                             const base::DictValue&,
                             std::vector<blink::MessagePortDescriptor>)) {
  auto* self = static_cast<Derived*>(this);
  RegisterHandler(
      action, [self, handler](const std::string& id, const base::DictValue& route,
                              const base::DictValue& payload,
                              std::vector<blink::MessagePortDescriptor> ports) {
        (self->*handler)(id, route, payload, std::move(ports));
      });
}

template <typename Derived>
void NexusBridgeCapabilityHandler::Register(
    std::string_view action,
    std::string_view required_key,
    void (Derived::*handler)(const std::string&,
                             const base::DictValue&,
                             std::string,
                             std::vector<blink::MessagePortDescriptor>)) {
  auto* self = static_cast<Derived*>(this);
  RegisterHandler(
      action,
      [this, self, handler, required_key](
          const std::string& id, const base::DictValue& route,
          const base::DictValue& payload,
          std::vector<blink::MessagePortDescriptor> ports) {
        std::string required_arg;
        if (!CopyRequiredStringOrSendInvalidArgs(id, route, payload,
                                                 required_key,
                                                 &required_arg)) {
          return;
        }

        (self->*handler)(id, route, std::move(required_arg), std::move(ports));
      });
}

template <typename Derived>
void NexusBridgeCapabilityHandler::Register(
    std::string_view action,
    std::string_view required_key,
    void (Derived::*handler)(const std::string&,
                             const base::DictValue&,
                             std::string,
                             const base::DictValue&,
                             std::vector<blink::MessagePortDescriptor>)) {
  auto* self = static_cast<Derived*>(this);
  RegisterHandler(
      action,
      [this, self, handler, required_key](
          const std::string& id, const base::DictValue& route,
          const base::DictValue& payload,
          std::vector<blink::MessagePortDescriptor> ports) {
        std::string required_arg;
        if (!CopyRequiredStringOrSendInvalidArgs(id, route, payload,
                                                 required_key,
                                                 &required_arg)) {
          return;
        }

        (self->*handler)(id, route, std::move(required_arg), payload,
                         std::move(ports));
      });
}

template <typename Derived>
void NexusBridgeCapabilityHandler::Register(
    std::string_view action,
    std::string_view first_required_key,
    std::string_view second_required_key,
    void (Derived::*handler)(const std::string&,
                             const base::DictValue&,
                             std::string,
                             std::string,
                             std::vector<blink::MessagePortDescriptor>)) {
  auto* self = static_cast<Derived*>(this);
  RegisterHandler(
      action,
      [this, self, handler, first_required_key, second_required_key](
          const std::string& id, const base::DictValue& route,
          const base::DictValue& payload,
          std::vector<blink::MessagePortDescriptor> ports) {
        std::array<std::string, 2> required_args;
        if (!CopyRequiredStringsOrSendInvalidArgs(
                id, route, payload,
                {first_required_key, second_required_key}, &required_args)) {
          return;
        }

        (self->*handler)(id, route, std::move(required_args[0]),
                         std::move(required_args[1]), std::move(ports));
      });
}

template <typename Derived>
void NexusBridgeCapabilityHandler::Register(
    std::string_view action,
    std::string_view first_required_key,
    std::string_view second_required_key,
    void (Derived::*handler)(const std::string&,
                             const base::DictValue&,
                             std::string,
                             std::string,
                             const base::DictValue&,
                             std::vector<blink::MessagePortDescriptor>)) {
  auto* self = static_cast<Derived*>(this);
  RegisterHandler(
      action,
      [this, self, handler, first_required_key, second_required_key](
          const std::string& id, const base::DictValue& route,
          const base::DictValue& payload,
          std::vector<blink::MessagePortDescriptor> ports) {
        std::array<std::string, 2> required_args;
        if (!CopyRequiredStringsOrSendInvalidArgs(
                id, route, payload,
                {first_required_key, second_required_key}, &required_args)) {
          return;
        }

        (self->*handler)(id, route, std::move(required_args[0]),
                         std::move(required_args[1]), payload,
                         std::move(ports));
      });
}

template <size_t N>
bool NexusBridgeCapabilityHandler::CopyRequiredStringsOrSendInvalidArgs(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& payload,
    const std::array<std::string_view, N>& required_keys,
    std::array<std::string, N>* out) const {
  for (size_t i = 0; i < N; ++i) {
    if (!CopyRequiredStringOrSendInvalidArgs(id, route, payload,
                                             required_keys[i],
                                             &(*out)[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_CAPABILITY_HANDLER_H_
