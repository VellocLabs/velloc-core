// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/page_script/page_script_channel.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_view_util.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace nexus {

namespace {

PageScriptChannel::Outcome ErrorOutcome(std::string_view code,
                                        base::DictValue params = {}) {
  PageScriptChannel::Outcome out;
  out.error_code = std::string(code);
  out.error_params = std::move(params);
  return out;
}

// A command that did not complete: a protocol error carries its text; no
// response at all means the page or the session is gone.
PageScriptChannel::Outcome FailureOutcome(const base::DictValue* error) {
  if (!error) {
    return ErrorOutcome("no_document");
  }
  base::DictValue params;
  const std::string* message = error->FindString("message");
  params.Set("detail", message ? *message : std::string());
  return ErrorOutcome("cdp_error", std::move(params));
}

// Turns a `Runtime.evaluate` result into an outcome: the thrown error's text
// when the script threw, else the returned value (`none` for undefined).
PageScriptChannel::Outcome OutcomeFromEvaluate(const base::DictValue& result) {
  PageScriptChannel::Outcome out;
  if (const base::DictValue* exception = result.FindDict("exceptionDetails")) {
    std::string text;
    if (const base::DictValue* thrown = exception->FindDict("exception")) {
      if (const std::string* description = thrown->FindString("description")) {
        text = *description;
      }
    }
    if (text.empty()) {
      if (const std::string* summary = exception->FindString("text")) {
        text = *summary;
      }
    }
    out.exception = text.empty() ? "uncaught exception" : text;
    return out;
  }
  if (const base::DictValue* remote = result.FindDict("result")) {
    if (const base::Value* value = remote->Find("value")) {
      out.value = value->Clone();
    }
  }
  return out;
}

}  // namespace

PageScriptChannel::Outcome::Outcome() = default;
PageScriptChannel::Outcome::Outcome(Outcome&&) noexcept = default;
PageScriptChannel::Outcome& PageScriptChannel::Outcome::operator=(
    Outcome&&) noexcept = default;
PageScriptChannel::Outcome::~Outcome() = default;

// static
std::unique_ptr<PageScriptChannel> PageScriptChannel::Attach(
    content::WebContents* web_contents,
    bool expose_to_page,
    Delegate* delegate) {
  if (!web_contents) {
    return nullptr;
  }
  std::unique_ptr<PageScriptChannel> channel(
      new PageScriptChannel(web_contents, expose_to_page, delegate));
  if (!channel->Start()) {
    return nullptr;
  }
  return channel;
}

PageScriptChannel::PageScriptChannel(content::WebContents* web_contents,
                                     bool expose_to_page,
                                     Delegate* delegate)
    : content::WebContentsObserver(web_contents),
      delegate_(delegate),
      expose_to_page_(expose_to_page) {}

PageScriptChannel::~PageScriptChannel() {
  detached_ = true;
  if (agent_host_) {
    agent_host_->DetachClient(this);
    agent_host_ = nullptr;
  }
  FailPending();
}

bool PageScriptChannel::Start() {
  agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents());
  if (!agent_host_ || !agent_host_->AttachClient(this)) {
    agent_host_ = nullptr;
    return false;
  }
  Send("Page.enable", {}, base::DoNothing());
  Send("Runtime.enable", {}, base::DoNothing());

  base::DictValue binding;
  binding.Set("name", kPageScriptBindingName);
  binding.Set("executionContextName", kPageScriptWorldName);
  Send("Runtime.addBinding", std::move(binding), base::DoNothing());

  base::DictValue bootstrap;
  bootstrap.Set("source",
                PageScriptBootstrapSource(PageScriptWorld::kIsolated));
  bootstrap.Set("worldName", kPageScriptWorldName);
  bootstrap.Set("runImmediately", true);
  Send("Page.addScriptToEvaluateOnNewDocument", std::move(bootstrap),
       base::DoNothing());

  if (expose_to_page_) {
    base::DictValue page_binding;
    page_binding.Set("name", kPageScriptPageBindingName);
    Send("Runtime.addBinding", std::move(page_binding), base::DoNothing());
    base::DictValue page_bootstrap;
    page_bootstrap.Set("source",
                       PageScriptBootstrapSource(PageScriptWorld::kMain));
    page_bootstrap.Set("runImmediately", true);
    Send("Page.addScriptToEvaluateOnNewDocument", std::move(page_bootstrap),
         base::DoNothing());
  }
  return true;
}

void PageScriptChannel::ExecuteScript(const std::string& code,
                                      PageScriptWorld world,
                                      OutcomeCallback done) {
  if (detached_) {
    std::move(done).Run(ErrorOutcome("no_document"));
    return;
  }
  if (world == PageScriptWorld::kMain) {
    Evaluate(code, std::nullopt, std::move(done));
    return;
  }
  WithIsolatedContext(base::BindOnce(
      [](base::WeakPtr<PageScriptChannel> self, std::string code,
         OutcomeCallback done, std::optional<int> context_id) {
        if (!self) {
          std::move(done).Run(ErrorOutcome("no_document"));
          return;
        }
        if (!context_id) {
          std::move(done).Run(ErrorOutcome("no_document"));
          return;
        }
        self->Evaluate(code, context_id, std::move(done));
      },
      weak_factory_.GetWeakPtr(), code, std::move(done)));
}

void PageScriptChannel::PostMessage(const std::string& json,
                                    OutcomeCallback done) {
  ExecuteScript(PageScriptDispatchExpression(json), PageScriptWorld::kIsolated,
                std::move(done));
}

void PageScriptChannel::AddInitScript(const std::string& source,
                                      OutcomeCallback done) {
  base::DictValue params;
  params.Set("source", source);
  params.Set("worldName", kPageScriptWorldName);
  Send(
      "Page.addScriptToEvaluateOnNewDocument", std::move(params),
      base::BindOnce(
          [](base::WeakPtr<PageScriptChannel> self, OutcomeCallback done,
             const base::DictValue* result, const base::DictValue* error) {
            const std::string* identifier =
                result ? result->FindString("identifier") : nullptr;
            if (!identifier) {
              std::move(done).Run(FailureOutcome(error));
              return;
            }
            if (self) {
              self->init_scripts_.insert(*identifier);
            }
            Outcome out;
            out.value = base::Value(*identifier);
            std::move(done).Run(std::move(out));
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void PageScriptChannel::RemoveInitScript(const std::string& script_id,
                                         OutcomeCallback done) {
  if (init_scripts_.erase(script_id) == 0) {
    Outcome out;
    out.value = base::Value(false);
    std::move(done).Run(std::move(out));
    return;
  }
  base::DictValue params;
  params.Set("identifier", script_id);
  Send("Page.removeScriptToEvaluateOnNewDocument", std::move(params),
       base::BindOnce(
           [](OutcomeCallback done, const base::DictValue* result,
              const base::DictValue* error) {
             if (error) {
               std::move(done).Run(FailureOutcome(error));
               return;
             }
             Outcome out;
             out.value = base::Value(true);
             std::move(done).Run(std::move(out));
           },
           std::move(done)));
}

void PageScriptChannel::DispatchProtocolMessage(
    content::DevToolsAgentHost* /*agent_host*/,
    base::span<const uint8_t> message) {
  if (detached_) {
    return;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      base::as_string_view(message), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;
  }
  const base::DictValue& dict = parsed->GetDict();
  if (std::optional<int> id = dict.FindInt("id")) {
    auto it = pending_.find(*id);
    if (it == pending_.end()) {
      return;
    }
    ResponseCallback callback = std::move(it->second);
    pending_.erase(it);
    std::move(callback).Run(dict.FindDict("result"), dict.FindDict("error"));
    return;
  }
  const std::string* method = dict.FindString("method");
  const base::DictValue* params = dict.FindDict("params");
  if (method && params) {
    OnEvent(*method, *params);
  }
}

void PageScriptChannel::AgentHostClosed(
    content::DevToolsAgentHost* /*agent_host*/) {
  // The host has already dropped this client.
  agent_host_ = nullptr;
  Detach();
}

void PageScriptChannel::WebContentsDestroyed() {
  Detach();
}

bool PageScriptChannel::Send(std::string_view method,
                             base::DictValue params,
                             ResponseCallback on_response) {
  if (detached_ || !agent_host_) {
    std::move(on_response).Run(nullptr, nullptr);
    return false;
  }
  const int id = next_id_++;
  base::DictValue envelope;
  envelope.Set("id", id);
  envelope.Set("method", method);
  envelope.Set("params", std::move(params));
  std::string serialized;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)),
                               &serialized)) {
    std::move(on_response).Run(nullptr, nullptr);
    return false;
  }
  pending_[id] = std::move(on_response);
  agent_host_->DispatchProtocolMessage(this, base::as_byte_span(serialized));
  return true;
}

void PageScriptChannel::OnEvent(std::string_view method,
                                const base::DictValue& params) {
  if (method == "Runtime.bindingCalled") {
    const std::string* name = params.FindString("name");
    const std::string* payload = params.FindString("payload");
    if (!name || !payload) {
      return;
    }
    PageScriptWorld world;
    if (*name == kPageScriptBindingName) {
      world = PageScriptWorld::kIsolated;
    } else if (*name == kPageScriptPageBindingName && expose_to_page_) {
      world = PageScriptWorld::kMain;
    } else {
      return;
    }
    if (!IsPageScriptPayloadWithinLimit(*payload)) {
      LOG(WARNING) << "pageScript: dropped a message above the size limit";
      return;
    }
    // Security: only a main-frame context may speak for the page. The
    // bootstrap also starts in same-process subframes; a message from one
    // would otherwise carry the top frame's origin.
    std::optional<int> context_id = params.FindInt("executionContextId");
    auto context = context_id ? contexts_.find(*context_id) : contexts_.end();
    if (context == contexts_.end() ||
        context->second.frame_id != MainFrameId()) {
      return;
    }
    base::DictValue message;
    message.Set("data", *payload);
    message.Set("world", std::string(PageScriptWorldName(world)));
    message.Set("origin", context->second.origin);
    content::RenderFrameHost* frame =
        web_contents() ? web_contents()->GetPrimaryMainFrame() : nullptr;
    message.Set("frameUrl",
                frame ? frame->GetLastCommittedURL().spec() : std::string());
    delegate_->OnPageScriptMessage(this, std::move(message));
    return;
  }
  if (method == "Runtime.executionContextCreated") {
    const base::DictValue* context = params.FindDict("context");
    std::optional<int> id = context ? context->FindInt("id") : std::nullopt;
    if (!id) {
      return;
    }
    const std::string* name = context->FindString("name");
    const std::string* origin = context->FindString("origin");
    const base::DictValue* aux = context->FindDict("auxData");
    const std::string* frame_id = aux ? aux->FindString("frameId") : nullptr;
    contexts_[*id] = ContextInfo{frame_id ? *frame_id : std::string(),
                                 origin ? *origin : std::string()};
    if (name && *name == kPageScriptWorldName && frame_id &&
        *frame_id == MainFrameId()) {
      isolated_context_id_ = id;
    }
    return;
  }
  if (method == "Runtime.executionContextDestroyed") {
    std::optional<int> id = params.FindInt("executionContextId");
    if (id) {
      contexts_.erase(*id);
    }
    if (isolated_context_id_ && id == isolated_context_id_) {
      isolated_context_id_.reset();
    }
    return;
  }
  if (method == "Runtime.executionContextsCleared") {
    contexts_.clear();
    isolated_context_id_.reset();
  }
}

void PageScriptChannel::WithIsolatedContext(
    base::OnceCallback<void(std::optional<int> context_id)> then) {
  if (isolated_context_id_) {
    std::move(then).Run(isolated_context_id_);
    return;
  }
  base::DictValue params;
  params.Set("frameId", MainFrameId());
  params.Set("worldName", kPageScriptWorldName);
  Send(
      "Page.createIsolatedWorld", std::move(params),
      base::BindOnce(
          [](base::WeakPtr<PageScriptChannel> self,
             base::OnceCallback<void(std::optional<int>)> then,
             const base::DictValue* result, const base::DictValue* /*error*/) {
            std::optional<int> id =
                result ? result->FindInt("executionContextId") : std::nullopt;
            if (self && id) {
              self->isolated_context_id_ = id;
            }
            std::move(then).Run(id);
          },
          weak_factory_.GetWeakPtr(), std::move(then)));
}

void PageScriptChannel::Evaluate(const std::string& expression,
                                 std::optional<int> context_id,
                                 OutcomeCallback done) {
  base::DictValue params;
  params.Set("expression", expression);
  params.Set("awaitPromise", true);
  params.Set("returnByValue", true);
  if (context_id) {
    params.Set("contextId", *context_id);
  }
  Send(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(
          [](OutcomeCallback done, const base::DictValue* result,
             const base::DictValue* error) {
            if (!result) {
              std::move(done).Run(FailureOutcome(error));
              return;
            }
            std::move(done).Run(OutcomeFromEvaluate(*result));
          },
          std::move(done)));
}

void PageScriptChannel::Detach() {
  if (detached_) {
    return;
  }
  detached_ = true;
  if (agent_host_) {
    agent_host_->DetachClient(this);
    agent_host_ = nullptr;
  }
  // Every open request answers before the delegate hears of the loss.
  FailPending();
  Observe(nullptr);
  delegate_->OnPageScriptDetached(this);
}

void PageScriptChannel::FailPending() {
  std::map<int, ResponseCallback> pending = std::move(pending_);
  for (auto& [id, callback] : pending) {
    std::move(callback).Run(nullptr, nullptr);
  }
}

std::string PageScriptChannel::MainFrameId() const {
  content::WebContents* contents = web_contents();
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  return frame ? frame->GetDevToolsFrameToken().ToString() : std::string();
}

}  // namespace nexus
