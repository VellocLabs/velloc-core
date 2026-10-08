#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_EVALUATE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_EVALUATE_H_

// One-shot CDP `Runtime.evaluate` driver for the BrowserEvaluate tool.
//
// Why DevTools (vs. RenderFrameHost::ExecuteJavaScript*):
//   - `ExecuteJavaScript`           : restricted to chrome://, devtools:// only.
//   - `ExecuteJavaScriptInIsolated…`: works on any page but only in an
//                                     isolated world — can't see page-side
//                                     globals (React/Redux/window.*), no
//                                     `awaitPromise` support.
//   - `ExecuteJavaScriptForTests`   : header explicitly says "ONLY FOR TESTS".
//   - DevTools `Runtime.evaluate`   : main world, awaitPromise + returnByValue
//                                     supported, production API. Chosen here.
//
// Lifecycle: a `BrowserEvaluateSession` attaches itself to the active tab's
// `DevToolsAgentHost`, sends a single `Runtime.evaluate` command, awaits the
// response (or the agent host closing), invokes its `ResultCallback`, detaches,
// and self-destructs via `delete this`. Callers do not own it.

#include <cstdint>
#include <string>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace content {
class WebContents;
}  // namespace content

namespace nexus {

// Callback shape: `result` is the parsed CDP response object (the contents of
// the JSON-RPC `result` field, *not* re-wrapped). On transport error / agent
// host close / non-attached, `error` is set instead. Exactly one of the two
// fields is non-empty when the callback fires.
struct EvaluateOutcome {
  // The CDP `Runtime.evaluate` result envelope (see CDP docs). Top-level keys
  // we care about: `result` (the RemoteObject), `exceptionDetails` (if the JS
  // threw). Empty when transport failed before any response arrived.
  base::DictValue result;
  // Non-empty on transport / attach failure. Surfaces as the tool's
  // `Reject(error_code, params)` at the call site; `error_params` carries
  // only data (the CDP error text under `detail`), never our own prose.
  std::string error_code;
  base::DictValue error_params;
};

class BrowserEvaluateSession : public content::DevToolsAgentHostClient {
 public:
  using ResultCallback = base::OnceCallback<void(EvaluateOutcome)>;

  // Constructs, attaches, and immediately sends the `Runtime.evaluate` command.
  // The session deletes itself once `done` fires. The caller may NOT delete
  // it. On any failure (no WebContents, attach refused, host closed mid-flight)
  // `done` is invoked with `error_code` populated.
  //
  // `code` is passed through as the `expression` param; the call always uses
  // `awaitPromise: true, returnByValue: true, allowUnsafeEvalBlockedByCSP:
  // false`. (We do NOT bypass page CSP — if a page denies eval, the model gets
  // a CSP exception, not a silent escape.)
  static void Start(content::WebContents* web_contents,
                    const std::string& code,
                    ResultCallback done);

  BrowserEvaluateSession(const BrowserEvaluateSession&) = delete;
  BrowserEvaluateSession& operator=(const BrowserEvaluateSession&) = delete;

 private:
  BrowserEvaluateSession(content::WebContents* web_contents,
                         const std::string& code,
                         ResultCallback done);
  ~BrowserEvaluateSession() override;

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

  // Sends the CDP envelope and bumps `next_id_`. Returns false on serialize
  // failure (shouldn't happen — pure JSON).
  bool SendCommand(const std::string& method, base::DictValue params);

  // Fires `done_` exactly once, detaches, and deletes `this`. Safe to call
  // from any of the callback paths (response received, host closed, attach
  // refused). Subsequent calls are no-ops.
  void Finish(EvaluateOutcome outcome);

  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  ResultCallback done_;
  // Single in-flight message id. The session only sends one `Runtime.evaluate`
  // command, so we only track the one id we're waiting for.
  int evaluate_id_ = 0;
  int next_id_ = 1;
  bool finished_ = false;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_BROWSER_EVALUATE_H_
