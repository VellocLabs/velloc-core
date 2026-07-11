#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_CALLBACK_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_CALLBACK_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"

// `ToolCallback` — the async resolver passed to every macro-DSL tool method.
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
//   - fail with `std::move(done).Reject("description")`.
//
// Resolve / Reject are `&&`-qualified — single-fire by construction. If
// the adapter weak ptr captured by the inner callback has been
// invalidated when the callback fires (adapter destroyed), the response
// is silently dropped — `base::OnceCallback` + `base::WeakPtr` already
// implements that behaviour. If the callback is destroyed without ever
// firing, its destructor runs the error callback with `tool_dropped` so
// the guest's pending promise never leaks.

namespace nexus {

class ToolCallback {
 public:
  using SuccessCallback = base::OnceCallback<void(base::Value result)>;
  using ErrorCallback =
      base::OnceCallback<void(std::string code, std::string message)>;

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

  // Error completion with the default error code "tool_error".
  void Reject(std::string message) &&;

  // Error completion with a caller-chosen error code.
  void Reject(std::string code, std::string message) &&;

  // True once Resolve / Reject has run. Mostly useful for tests.
  bool fired() const { return fired_; }

 private:
  void DispatchSuccess(base::Value value);
  void DispatchError(std::string code, std::string message);

  SuccessCallback on_success_;
  ErrorCallback on_error_;
  bool fired_ = false;
};

// Wrap a `ToolCallback` with a wall-clock budget so a hung actor
// call can't deadlock the loop. Returns a fresh `ToolCallback` whose
// Resolve / Reject path forwards to the wrapped inner callback,
// canceling the timer first; if the timer fires before either path
// runs, it rejects the inner with a `tool_action_timeout` error code.
//
// First-write-wins: whichever of (Resolve, Reject, OnTimeout) runs
// first settles the inner callback; the others become no-ops. This
// mirrors the Browser-Use reference's `action_timeout` semantics.
//
// `budget <= 0` short-circuits — returns the inner callback unchanged
// so callers can pass `BudgetWrap(done, base::TimeDelta())` to
// disable budgeting per-call.
ToolCallback BudgetWrap(ToolCallback inner,
                        base::TimeDelta budget,
                        std::string action_name);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_CALLBACK_H_
