// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog_watcher.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/web_contents.h"

namespace nexus {

JsDialogWatcher::JsDialogWatcher(content::WebContents* web_contents,
                                 int32_t tab_id,
                                 Delegate* delegate)
    : content::WebContentsObserver(web_contents),
      tab_id_(tab_id),
      delegate_(delegate) {}

JsDialogWatcher::~JsDialogWatcher() {
  if (agent_host_) {
    agent_host_->DetachClient(this);
    agent_host_ = nullptr;
  }
  // Answered in a later task: the owner may be erasing this watcher from the
  // container an answer's follow-up prunes.
  std::map<int, AnswerCallback> pending = std::move(pending_);
  for (auto& [id, callback] : pending) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), false));
  }
}

bool JsDialogWatcher::Start() {
  if (!web_contents()) {
    return false;
  }
  agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents());
  if (!agent_host_ || !agent_host_->AttachClient(this)) {
    agent_host_ = nullptr;
    return false;
  }
  Send("Page.enable", {}, base::DoNothing());
  return true;
}

void JsDialogWatcher::Answer(bool accept, AnswerCallback done) {
  if (!open_dialog_ || !agent_host_) {
    std::move(done).Run(false);
    return;
  }
  base::DictValue params;
  params.Set("accept", accept);
  if (open_dialog_->type == "prompt" && accept) {
    params.Set("promptText", open_dialog_->prompt_text.value_or(
                                 open_dialog_->default_prompt));
  }
  // On success the dialog is gone even if `javascriptDialogClosed` has not
  // arrived yet, so the follow-up observation is not refused.
  Send("Page.handleJavaScriptDialog", std::move(params),
       base::BindOnce(
           [](base::WeakPtr<JsDialogWatcher> self, AnswerCallback done,
              bool ok) {
             if (ok && self) {
               self->open_dialog_.reset();
             }
             std::move(done).Run(ok);
           },
           weak_ptr_factory_.GetWeakPtr(), std::move(done)));
}

void JsDialogWatcher::DispatchProtocolMessage(
    content::DevToolsAgentHost* /*agent_host*/,
    base::span<const uint8_t> message) {
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
    AnswerCallback callback = std::move(it->second);
    pending_.erase(it);
    std::move(callback).Run(dict.FindDict("error") == nullptr);
    return;
  }
  const std::string* method = dict.FindString("method");
  const base::DictValue* params = dict.FindDict("params");
  if (!method || !params) {
    return;
  }
  if (*method == "Page.javascriptDialogOpening") {
    open_dialog_ = ParseJsDialogOpening(*params);
    if (open_dialog_) {
      delegate_->OnJsDialogOpened(tab_id_);
    }
  } else if (*method == "Page.javascriptDialogClosed") {
    if (open_dialog_) {
      open_dialog_.reset();
      delegate_->OnJsDialogClosed(tab_id_);
    }
  }
}

void JsDialogWatcher::AgentHostClosed(
    content::DevToolsAgentHost* /*agent_host*/) {
  agent_host_ = nullptr;
  Detach();
}

void JsDialogWatcher::WebContentsDestroyed() {
  Detach();
}

void JsDialogWatcher::Send(std::string_view method,
                           base::DictValue params,
                           AnswerCallback on_response) {
  if (!agent_host_) {
    std::move(on_response).Run(false);
    return;
  }
  const int id = next_id_++;
  base::DictValue envelope;
  envelope.Set("id", id);
  envelope.Set("method", method);
  envelope.Set("params", std::move(params));
  std::string serialized;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)),
                               &serialized)) {
    std::move(on_response).Run(false);
    return;
  }
  pending_[id] = std::move(on_response);
  agent_host_->DispatchProtocolMessage(this, base::as_byte_span(serialized));
}

// An open request's answer can lead the delegate to prune detached watchers,
// this one included, so the answers run last and touch no member.
void JsDialogWatcher::Detach() {
  if (agent_host_) {
    agent_host_->DetachClient(this);
    agent_host_ = nullptr;
  }
  Observe(nullptr);
  if (open_dialog_) {
    open_dialog_.reset();
    delegate_->OnJsDialogClosed(tab_id_);
  }
  std::map<int, AnswerCallback> pending = std::move(pending_);
  pending_.clear();
  for (auto& [id, callback] : pending) {
    std::move(callback).Run(false);
  }
}

}  // namespace nexus
