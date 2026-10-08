#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_CALLBACK_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_CALLBACK_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

// `ToolCallback` — the async resolver passed to every host tool method.
//
// The framework-generated dispatcher pre-binds the request id, the route
// dict, and a weak pointer to the owning adapter into two `base::OnceCallback`s
// (one for success, one for error), then packages them into a `ToolCallback`
// instance handed to the tool body as its trailing argument:
//
//     void example_create_tab(std::string url,
//                             std::optional<int> window_id,
//                             ToolCallback done);
//
// The tool body is free to:
//   - resolve immediately:  `std::move(done).Resolve(base::Value(...));`
//   - hop sequences before resolving (PostTask, BindOnce);
//   - chain through the actor service / network / DOM observation and
//     resolve from a callback minutes later;
//   - fail with `std::move(done).Reject("code", params)` — a code the
//     action's `.api.json` declares; the runtime renders the text.
//
// Resolve / Reject are `&&`-qualified — single-fire by construction. If
// the adapter weak ptr captured by the inner callback has been
// invalidated when the callback fires (adapter destroyed), the response
// is silently dropped — `base::OnceCallback` + `base::WeakPtr` already
// implements that behaviour. If the callback is destroyed without ever
// firing, its destructor runs the error callback with `tool_dropped` so
// the guest's pending promise never leaks.

namespace nexus {

// The `params` for a failure a service reported before the tool's own work
// started (the shell launcher's BINARY_NOT_FOUND / INVALID_CWD, the ripgrep
// runner's pre-launch error). The tool rejects with its OWN declared code;
// the service's code travels as `service_code` so the runtime's fallback
// (pwsh → powershell) can still read it, next to the service's sentence in
// `detail` and, when present, the manual `install_hint`.
base::DictValue ServiceFailureParams(
    const std::optional<std::string>& service_code,
    const std::optional<std::string>& detail,
    const std::optional<std::string>& install_hint);

class ToolCallback {
 public:
  using SuccessCallback = base::OnceCallback<void(base::Value result)>;
  // `params` is the error's structured detail (`{code, params}` is what the
  // runtime renders text from; the host sends no model-visible prose).
  // `message` repeats the code for clients that read only the message.
  using ErrorCallback = base::OnceCallback<
      void(std::string code, std::string message, base::DictValue params)>;

  // Default-constructs an inert callback. Used only by std::move targets
  // and tests; the real construction path is the dispatcher.
  ToolCallback();

  // Constructed by the dispatcher with pre-bound success and error
  // callbacks. Both callbacks should already capture (id, route, weak
  // adapter) so the tool body never has to think about correlation or
  // lifetime.
  ToolCallback(SuccessCallback on_success, ErrorCallback on_error);

  ~ToolCallback();

  ToolCallback(const ToolCallback&) = delete;
  ToolCallback& operator=(const ToolCallback&) = delete;

  ToolCallback(ToolCallback&&) noexcept;
  ToolCallback& operator=(ToolCallback&&) noexcept;

  // Successful completion. Runs the captured success callback with the
  // given value, which the dispatcher's bound shim turns into
  // `SendSuccessResponse(id, route, value)` on the adapter.
  void Resolve(base::Value value) &&;

  // Convenience overloads — avoid the `base::Value(std::move(...))` wrap
  // at every call site.
  void Resolve(base::DictValue value) &&;
  void Resolve(base::ListValue value) &&;

  // Convenience for tools that don't return data — sends `{ ok: true }`
  // so the JS guest gets a stable, non-null result shape.
  void ResolveVoid() &&;

  // A declared error code with its parameters (`{code, params}`); the
  // message is the code itself — the runtime renders the text.
  void Reject(std::string code, base::DictValue params) &&;

  // True once Resolve / Reject has run. Mostly useful for tests.
  bool fired() const { return fired_; }

 private:
  void DispatchSuccess(base::Value value);
  void DispatchError(std::string code,
                     std::string message,
                     base::DictValue params);

  SuccessCallback on_success_;
  ErrorCallback on_error_;
  bool fired_ = false;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_CALLBACK_H_
