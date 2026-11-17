#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "chrome/browser/profiles/profile.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_evaluate.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

// Soft cap on the JSON-stringified value returned to the model. Mirrors
// browser-use's 20k cap (they truncate after serialization too). Anything
// past this is replaced with a "[truncated]" sentinel rather than silently
// cut mid-token so the model can detect when it lost data.
constexpr size_t kEvaluateResultMaxBytes = 20 * 1024;

// Stringifies a base::Value into JSON, then truncates to the soft cap.
// Returns `truncated` so the caller can flag it on the observation.
std::string SerializeAndTruncateForEvaluate(const base::Value& value,
                                            bool* truncated_out) {
  std::string serialized;
  if (!base::JSONWriter::Write(value, &serialized)) {
    *truncated_out = false;
    return std::string("\"[unserializable]\"");
  }
  if (serialized.size() <= kEvaluateResultMaxBytes) {
    *truncated_out = false;
    return serialized;
  }
  // Keep the head; the model can re-run a narrower query if it lost data.
  serialized.resize(kEvaluateResultMaxBytes);
  serialized.append("…[truncated]");
  *truncated_out = true;
  return serialized;
}

// Builds the success observation from a CDP Runtime.evaluate result:
// `exceptionDetails` → `error` + `exception` (still ok:true); `result.value` →
// JSON-stringified and capped; otherwise `result.description`.
base::DictValue BuildEvaluateObservation(base::DictValue cdp_result) {
  base::DictValue out;
  out.Set("ok", true);
  out.Set("action", "evaluate");

  if (base::DictValue* exc = cdp_result.FindDict("exceptionDetails")) {
    // The JS threw. We pass `ok:true` (the tool *call* succeeded — only the
    // user-supplied JS failed) but surface the exception verbatim so the model
    // can read the stack and adapt.
    // The page's own exception text when it gave one; the runtime renders
    // the fallback for a bare `exception`.
    if (const std::string* text = exc->FindString("text")) {
      out.Set("error", *text);
    }
    out.Set("exception", base::Value(std::move(*exc)));
    return out;
  }

  base::DictValue* obj = cdp_result.FindDict("result");
  if (!obj) {
    out.Set("result", std::string());
    out.Set("type", "undefined");
    return out;
  }
  const std::string* type = obj->FindString("type");
  if (type) {
    out.Set("type", *type);
  }
  if (const std::string* subtype = obj->FindString("subtype")) {
    out.Set("subtype", *subtype);
  }

  if (base::Value* value = obj->Find("value")) {
    bool truncated = false;
    std::string serialized =
        SerializeAndTruncateForEvaluate(*value, &truncated);
    out.Set("result", std::move(serialized));
    if (truncated) {
      out.Set("truncated", true);
    }
  } else if (const std::string* description = obj->FindString("description")) {
    // Non-serializable (function, DOM node, etc.). The description is CDP's
    // preview string ("function foo() { ... }", "HTMLDivElement", etc.) —
    // useful to the model as a "this was here but I can't see it" signal.
    out.Set("result", *description);
  } else {
    out.Set("result", std::string());
  }
  return out;
}

}  // namespace

void BrowserTool::RunEvaluateOnUI(::nexus::generated::BrowserEvaluateInput input,
                                  ToolCallback done) {
  // BrowserEvaluate is a first-class browser tool, always available like
  // the rest of the Browser* surface — no opt-in gate. (The former
  // `nexus.browser_tools_allow_evaluate` pref + Settings · Tools toggle +
  // `ShouldAllowAgentEvaluate` gate were removed.)
  if (input.code.empty()) {
    std::move(done).Reject("invalid_args", ReasonParams("code_required"));
    return;
  }

  // Resolve target tab. evaluate, like read_page/screenshot, always runs on
  // the active session-scoped tab — there's no per-call tab override.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "evaluate", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(*resolved_tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    {
      base::DictValue reject_params;
      reject_params.Set("tool", "evaluate");
      reject_params.Set("tab_id", *resolved_tab_id);
      std::move(done).Reject("tab_not_found", std::move(reject_params));
    }
    return;
  }

  // Hand off to the one-shot CDP session. The session attaches the
  // DevToolsAgentHost, sends Runtime.evaluate (awaitPromise=true,
  // returnByValue=true), and self-destructs on completion. Captured
  // `tab_id` and `done` are moved through to the reply closure.
  const int32_t tab_id = *resolved_tab_id;
  WatchJsDialogs(tab_id);
  if (RefuseWhileJsDialogOpen(tab_id, "evaluate", done)) {
    return;
  }
  done = TrackInFlight(std::move(done), "evaluate", tab_id);
  BrowserEvaluateSession::Start(
      contents, input.code,
      base::BindOnce(
          [](int32_t tab_id, ToolCallback inner_done, EvaluateOutcome outcome) {
            if (!outcome.error_code.empty()) {
              std::move(inner_done)
                  .Reject(std::move(outcome.error_code),
                          std::move(outcome.error_params));
              return;
            }
            base::DictValue observation =
                BuildEvaluateObservation(std::move(outcome.result));
            observation.Set("tab_id", tab_id);
            std::move(inner_done).Resolve(std::move(observation));
          },
          tab_id, std::move(done)));
}

}  // namespace nexus
