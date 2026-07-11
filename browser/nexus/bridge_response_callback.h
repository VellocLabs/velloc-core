#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BRIDGE_RESPONSE_CALLBACK_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BRIDGE_RESPONSE_CALLBACK_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

class NexusBridgeCapabilityHandler;

// Move-only response handle handed to every bridge action implementation.
// Methods are safe to call from ANY thread — internally the callback
// PostTasks to UI before invoking the matching Send*Response on the
// owning NexusBridgeCapabilityHandler. Implementations therefore never
// have to PostTask manually for the response hop.
//
// Modeled after `ToolCallback` for tools (see nexus_tool_callback.h)
// but with the four bridge-specific terminal methods:
//
//     std::move(done).Success(base::Value(...));
//     std::move(done).Error("FS_ERROR", "stat failed");
//     std::move(done).InvalidArgs("missing field 'path'");
//     std::move(done).Binary(envelope, std::move(data), "$result");
//
// All four are `&&`-qualified — single-fire by construction. If the
// owning handler's WeakPtr has been invalidated when the reply runs
// (the bridge port closed during in-flight work), the response is
// silently dropped — guest's pending promise gets dropped at the JS
// layer rather than being resolved to garbage.
//
// The callback is move-only (matches OnceCallback's idiom) but
// MOVABLE — implementations can store it (e.g. into a binary-port
// pending map) and fire it later when async data arrives.
class BridgeResponseCallback {
 public:
  // Default-constructed inert callback. Used only as a `std::move`
  // target / for storage slots that are filled later.
  BridgeResponseCallback();

  ~BridgeResponseCallback();

  BridgeResponseCallback(const BridgeResponseCallback&) = delete;
  BridgeResponseCallback& operator=(const BridgeResponseCallback&) = delete;
  BridgeResponseCallback(BridgeResponseCallback&&) noexcept;
  BridgeResponseCallback& operator=(BridgeResponseCallback&&) noexcept;

  // Construct from a handler weak ptr + correlation state. Public so the
  // codegen-emitted dispatcher can mint these; in practice no one else
  // calls this directly.
  BridgeResponseCallback(
      base::WeakPtr<NexusBridgeCapabilityHandler> handler,
      std::string id,
      base::DictValue route);

  // ---- Terminal methods --------------------------------------------------
  // Each fires exactly once. Subsequent calls (after std::move'ing) are
  // no-ops on the moved-from object (its `fired_` flag is true / its
  // handler is empty).

  // Success with an arbitrary value payload.
  void Success(base::Value value) &&;

  // Convenience overloads — let callers skip the `base::Value(...)` wrap.
  void Success(base::DictValue dict) &&;
  void Success(base::ListValue list) &&;

  // Success with no payload — sends `{ ok: true }` so the JS guest's
  // result has a stable non-null shape.
  void SuccessVoid() &&;

  // Generic error. `code` is the symbolic error tag (e.g. "FS_ERROR",
  // "LLM_ERROR"); `message` is the human-readable detail.
  void Error(std::string code, std::string message) &&;

  // Invalid-args specialization. Maps to SendInvalidArgsResponse, which
  // is wire-distinct from a generic error so the JS guest can route it
  // to argument-validation handling.
  void InvalidArgs(std::string message) &&;

  // Binary response: `envelope` is the JSON metadata, `data` is the raw
  // bytes, `binary_field` is the dotted key under which the bytes
  // surface in the envelope (e.g. "$result", "data").
  void Binary(base::Value envelope,
              mojo_base::BigBuffer data,
              std::string binary_field) &&;

  // True after any terminal method has been called. Useful for tests
  // and for asserting "exactly one fire".
  bool fired() const { return fired_; }

  // Read-only access to the wire request id. Useful for implementations
  // that need to key pending state (e.g. binary-port endpoints) by the
  // originating request — most callers don't need this.
  const std::string& id() const { return id_; }

 private:
  // Each Dispatch* runs on UI (PostTask'd from the corresponding
  // public terminal). They invoke the matching Send*Response on the
  // captured handler, which checks `delegate_` validity itself.
  void DispatchSuccess(base::Value value);
  void DispatchError(std::string code, std::string message);
  void DispatchInvalidArgs(std::string message);
  void DispatchBinary(base::Value envelope,
                      mojo_base::BigBuffer data,
                      std::string binary_field);

  base::WeakPtr<NexusBridgeCapabilityHandler> handler_;
  std::string id_;
  base::DictValue route_;
  bool fired_ = false;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BRIDGE_RESPONSE_CALLBACK_H_
