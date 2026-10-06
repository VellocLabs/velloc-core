#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_CAPABILITY_HANDLER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_CAPABILITY_HANDLER_H_

#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

namespace nexus {

class BridgeResponseCallback;
class NexusBridgeResponseDelegate;
class NexusMessagePortEndpoint;

// Result of a binary-port byte-receive operation. Returned via the
// callback passed to AwaitBinaryFromPort. `ok` distinguishes "got bytes"
// from "port disconnected / wrong payload type" — on `!ok`, `error`
// carries a one-line description suitable for InvalidArgs / Error.
struct BinaryReceiveResult {
  bool ok = false;
  std::vector<uint8_t> bytes;
  std::string error;

  // BigBuffer-free → trivially copyable, but we move because the bytes
  // can be large.
  BinaryReceiveResult();
  BinaryReceiveResult(BinaryReceiveResult&&) noexcept;
  BinaryReceiveResult& operator=(BinaryReceiveResult&&) noexcept;
  BinaryReceiveResult(const BinaryReceiveResult&) = delete;
  BinaryReceiveResult& operator=(const BinaryReceiveResult&) = delete;
  ~BinaryReceiveResult();
};

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

  // The guest end of the bridge port went away (its page reloaded, the port
  // was replaced, or it disconnected). The adapter itself lives on with the
  // port service, so per-connection resources — fs watches, open sockets —
  // are released here rather than in the destructor. Called on UI by
  // NexusBridgePortService; may run more than once and must be idempotent.
  // Default: nothing to release.
  virtual void OnBridgeConnectionClosed() {}

  // A guest connection began (NexusBridgePortService::CreatePortPair handed
  // out a fresh port). Pairs with OnBridgeConnectionClosed, which a replaced
  // port gets first. For state that must exist only while a guest can
  // receive events. Default: nothing.
  virtual void OnBridgeConnectionOpened() {}

  // Returns a WeakPtr<NexusBridgeCapabilityHandler> bound to this object's
  // home sequence (UI). Used by the codegen-emitted dispatcher to
  // manufacture BridgeResponseCallbacks; those callbacks are safe to
  // store / move across threads, but they always dereference back on
  // UI to invoke Send*Response.
  base::WeakPtr<NexusBridgeCapabilityHandler> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Receive a binary buffer from a transferred message port, then
  // invoke `on_done` with the result. Used by codegen-emitted
  // dispatchers when an action's input declares a `bytes` type and
  // the inline-payload variant wasn't sent (i.e. the guest chose to
  // transfer the bytes via a MessagePort instead of encoding them as
  // an array<integer> in the JSON payload).
  //
  // The callback fires exactly once on UI:
  //   - With `ok=true` and `bytes` populated when the port delivers a
  //     binary `MessageEvent` (an ArrayBuffer).
  //   - With `ok=false` and `error` populated if the port disconnects
  //     before delivering, or delivers a non-binary message.
  //
  // The endpoint and the callback are owned by this handler until the
  // callback fires; if THIS handler is destroyed first, the callback
  // is silently dropped (the pending entry destructs along with the
  // adapter).
  //
  // Thread: must be called on this object's home sequence (UI). The
  // callback also fires on UI.
  void AwaitBinaryFromPort(
      blink::MessagePortDescriptor port,
      base::OnceCallback<void(BinaryReceiveResult)> on_done);

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

  // BridgeResponseCallback needs to invoke our protected Send*Response
  // family from a UI-thread reply trampoline it owns. Friend the
  // callback rather than promoting the methods to public — keeps
  // direct call sites under the existing access discipline.
  friend class BridgeResponseCallback;

  // Per-pending-receive state for AwaitBinaryFromPort. The endpoint
  // and the on_done callback are stored together; whichever path
  // (OnMessage with bytes / OnError with disconnect) fires first
  // moves on_done out, runs it, and erases the entry.
  struct PendingBinaryReceive {
    std::unique_ptr<NexusMessagePortEndpoint> endpoint;
    base::OnceCallback<void(BinaryReceiveResult)> on_done;

    PendingBinaryReceive();
    PendingBinaryReceive(PendingBinaryReceive&&) noexcept;
    PendingBinaryReceive& operator=(PendingBinaryReceive&&) noexcept;
    PendingBinaryReceive(const PendingBinaryReceive&) = delete;
    PendingBinaryReceive& operator=(const PendingBinaryReceive&) = delete;
    ~PendingBinaryReceive();
  };

  // Resolve a pending receive (called by the closures inside
  // AwaitBinaryFromPort).
  void FinishPendingBinaryReceive(int receive_id, BinaryReceiveResult result);

  raw_ptr<NexusBridgeResponseDelegate> delegate_;
  std::map<std::string, DispatchHandler, std::less<>> handlers_;

  // Pending binary receives, keyed by an internal counter. Each entry
  // outlives its dispatcher closure until the port delivers bytes or
  // disconnects; on adapter destruction, this map clears, dropping any
  // unanswered binary receives cleanly.
  std::map<int, PendingBinaryReceive> pending_binary_receives_;
  int next_binary_receive_id_ = 0;

  // Last member per chromium-style. Used by AsWeakPtr() above to mint
  // WeakPtrs for the BridgeResponseCallback machinery.
  base::WeakPtrFactory<NexusBridgeCapabilityHandler> weak_factory_{this};
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
