#include "custom_browser/browser/nexus/tools/browser_tool/browser_evaluate.h"

#include <utility>

#include <string_view>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_view_util.h"
#include "content/public/browser/web_contents.h"

namespace nexus {

// static
void BrowserEvaluateSession::Start(content::WebContents* web_contents,
                                   const std::string& code,
                                   ResultCallback done) {
  if (!web_contents) {
    EvaluateOutcome out;
    out.error_code = "no_active_tab";
    out.error_params.Set("reason", "evaluate");
    std::move(done).Run(std::move(out));
    return;
  }
  // self-owning; deletes itself in Finish().
  new BrowserEvaluateSession(web_contents, code, std::move(done));
}

BrowserEvaluateSession::BrowserEvaluateSession(
    content::WebContents* web_contents,
    const std::string& code,
    ResultCallback done)
    : done_(std::move(done)) {
  agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents);
  if (!agent_host_) {
    EvaluateOutcome out;
    out.error_code = "agent_host_unavailable";
    Finish(std::move(out));
    return;
  }
  if (!agent_host_->AttachClient(this)) {
    EvaluateOutcome out;
    out.error_code = "attach_refused";
    // `agent_host_` will fall out of scope; nothing else to detach.
    Finish(std::move(out));
    return;
  }

  // Send the single Runtime.evaluate command.
  base::DictValue params;
  params.Set("expression", code);
  params.Set("awaitPromise", true);
  params.Set("returnByValue", true);
  // Bypass page CSP for the evaluate call. The agent surface is a privileged
  // automation knob (a first-class browser tool), and the model needs to run
  // helper IIFEs / DOM queries on pages whose own CSP forbids `eval` for
  // first-party scripts. CSP-respecting evaluation would silently fail on the
  // common case (any modern web app with a strict CSP) and force the model to
  // give up rather than complete the user's request.
  params.Set("allowUnsafeEvalBlockedByCSP", true);
  // Mark the evaluation as user-gestured. JS that gates on
  // navigator.userActivation / transient user activation (window.open,
  // permission prompts, autoplay, full-screen, copy-to-clipboard, etc.) only
  // runs from a real interaction otherwise. Since the agent is acting on the
  // user's behalf, simulating user activation matches the intent — the same
  // posture the actor's click pipeline already takes for synthesized clicks.
  params.Set("userGesture", true);
  // Don't enable replMode — the model has no REPL semantics.

  evaluate_id_ = next_id_;
  if (!SendCommand("Runtime.evaluate", std::move(params))) {
    EvaluateOutcome out;
    out.error_code = "send_failed";
    Finish(std::move(out));
    return;
  }
}

BrowserEvaluateSession::~BrowserEvaluateSession() = default;

bool BrowserEvaluateSession::SendCommand(const std::string& method,
                                         base::DictValue params) {
  CHECK(agent_host_);
  base::DictValue envelope;
  envelope.Set("id", next_id_++);
  envelope.Set("method", method);
  envelope.Set("params", std::move(params));
  std::string serialized;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &serialized)) {
    return false;
  }
  // base::as_byte_span carries the container's bounds info through to the
  // span, so Chromium's -Wunsafe-buffer-usage is satisfied (the equivalent
  // raw `reinterpret_cast<const uint8_t*>(data()), size()` constructor
  // dropped the bounds and is now an error). See docs/unsafe_buffers.md.
  agent_host_->DispatchProtocolMessage(this, base::as_byte_span(serialized));
  return true;
}

void BrowserEvaluateSession::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  if (finished_) {
    // Defensive: ignore any late events after we've already finished.
    return;
  }
  // CDP messages from the agent are UTF-8 JSON. (UsesBinaryProtocol() defaults
  // to false; we don't override it.) `base::as_string_view(span<const unsigned
  // char>)` is the bounds-aware conversion the spanification rules require —
  // a raw `reinterpret_cast<const char*>(data()), size()` constructor would
  // trigger -Wunsafe-buffer-usage.
  std::string_view text = base::as_string_view(message);
  // velloc's Chromium fork requires the explicit options arg on
  // JSONReader::Read; JSON_PARSE_RFC = strict standard parsing (no trailing
  // commas, no comments) — same setting the rest of nexus/ uses.
  std::optional<base::Value> parsed =
      base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;  // unparseable / event message we don't care about.
  }
  base::DictValue& dict = parsed->GetDict();

  // Match the response for our single Runtime.evaluate command.
  std::optional<int> id = dict.FindInt("id");
  if (!id.has_value() || *id != evaluate_id_) {
    return;  // some other event (e.g. Runtime.executionContextCreated).
  }

  EvaluateOutcome out;
  if (const base::DictValue* err = dict.FindDict("error")) {
    // CDP-level transport error (method not found, invalid params, etc.).
    out.error_code = "cdp_error";
    if (const std::string* msg = err->FindString("message")) {
      out.error_params.Set("detail", *msg);
    }
  } else if (const base::DictValue* result = dict.FindDict("result")) {
    // Normal completion path. Hand the result envelope to the caller — the
    // tool-side glue translates `result.value` / `exceptionDetails` into the
    // observation shape.
    out.result = result->Clone();
  } else {
    out.error_code = "cdp_no_result";
  }
  Finish(std::move(out));
}

void BrowserEvaluateSession::AgentHostClosed(
    content::DevToolsAgentHost* agent_host) {
  // Tab closed / navigation crashed / host shutdown mid-flight. The agent host
  // has already detached us — do NOT call DetachClient again from Finish().
  agent_host_ = nullptr;
  EvaluateOutcome out;
  if (finished_) {
    // Already replied; just self-delete.
    delete this;
    return;
  }
  out.error_code = "agent_host_closed";
  Finish(std::move(out));
}

void BrowserEvaluateSession::Finish(EvaluateOutcome outcome) {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (agent_host_) {
    agent_host_->DetachClient(this);
    agent_host_ = nullptr;
  }
  // Run the callback BEFORE deleting so the caller can chain without
  // re-entering through a deleted `this`.
  std::move(done_).Run(std::move(outcome));
  delete this;
}

}  // namespace nexus
