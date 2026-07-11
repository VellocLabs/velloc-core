#include "custom_browser/browser/nexus/bridge_response_callback.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "content/public/browser/browser_thread.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"

namespace nexus {

namespace {

base::Value BuildOkPayload() {
  base::DictValue ok;
  ok.Set("ok", true);
  return base::Value(std::move(ok));
}

}  // namespace

BridgeResponseCallback::BridgeResponseCallback() = default;

BridgeResponseCallback::~BridgeResponseCallback() = default;

BridgeResponseCallback::BridgeResponseCallback(
    BridgeResponseCallback&&) noexcept = default;

BridgeResponseCallback& BridgeResponseCallback::operator=(
    BridgeResponseCallback&&) noexcept = default;

BridgeResponseCallback::BridgeResponseCallback(
    base::WeakPtr<NexusBridgeCapabilityHandler> handler,
    std::string id,
    base::DictValue route)
    : handler_(std::move(handler)),
      id_(std::move(id)),
      route_(std::move(route)) {}

// ---- Terminal methods ------------------------------------------------------
//
// Each is `&&`-qualified — the call consumes the callback. We forward to
// a UI-thread Dispatch*() trampoline via PostTask so the rest of the
// stack (Send*Response → delegate_) runs on the bridge port service's
// home sequence. Calling from UI itself still goes through PostTask for
// uniformity; the cost is negligible (one queue entry) and the routing
// stays simple.

void BridgeResponseCallback::Success(base::Value value) && {
  if (fired_) {
    return;
  }
  fired_ = true;
  // We move `*this` into the bound callback so `handler_`, `id_`,
  // `route_` survive past the caller's stack frame.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&BridgeResponseCallback::DispatchSuccess,
                     base::Owned(new BridgeResponseCallback(std::move(*this))),
                     std::move(value)));
}

void BridgeResponseCallback::Success(base::DictValue dict) && {
  std::move(*this).Success(base::Value(std::move(dict)));
}

void BridgeResponseCallback::Success(base::ListValue list) && {
  std::move(*this).Success(base::Value(std::move(list)));
}

void BridgeResponseCallback::SuccessVoid() && {
  std::move(*this).Success(BuildOkPayload());
}

void BridgeResponseCallback::Error(std::string code, std::string message) && {
  if (fired_) {
    return;
  }
  fired_ = true;
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&BridgeResponseCallback::DispatchError,
                     base::Owned(new BridgeResponseCallback(std::move(*this))),
                     std::move(code), std::move(message)));
}

void BridgeResponseCallback::InvalidArgs(std::string message) && {
  if (fired_) {
    return;
  }
  fired_ = true;
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&BridgeResponseCallback::DispatchInvalidArgs,
                     base::Owned(new BridgeResponseCallback(std::move(*this))),
                     std::move(message)));
}

void BridgeResponseCallback::Binary(base::Value envelope,
                                    mojo_base::BigBuffer data,
                                    std::string binary_field) && {
  if (fired_) {
    return;
  }
  fired_ = true;
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&BridgeResponseCallback::DispatchBinary,
                     base::Owned(new BridgeResponseCallback(std::move(*this))),
                     std::move(envelope), std::move(data),
                     std::move(binary_field)));
}

// ---- UI-thread trampolines -------------------------------------------------
// Run on UI by virtue of how they were PostTask'd. Each forwards into
// the matching Send*Response on the captured handler if it's still
// alive (WeakPtr-checked); otherwise the response is silently dropped.

void BridgeResponseCallback::DispatchSuccess(base::Value value) {
  if (!handler_) {
    return;
  }
  handler_->SendSuccessResponse(id_, route_, std::move(value));
}

void BridgeResponseCallback::DispatchError(std::string code,
                                           std::string message) {
  if (!handler_) {
    return;
  }
  handler_->SendErrorResponse(id_, route_, code, message);
}

void BridgeResponseCallback::DispatchInvalidArgs(std::string message) {
  if (!handler_) {
    return;
  }
  handler_->SendInvalidArgsResponse(id_, route_, message);
}

void BridgeResponseCallback::DispatchBinary(base::Value envelope,
                                            mojo_base::BigBuffer data,
                                            std::string binary_field) {
  if (!handler_) {
    return;
  }
  handler_->SendBinaryResponse(id_, route_, std::move(envelope),
                               std::move(data), binary_field);
}

}  // namespace nexus
