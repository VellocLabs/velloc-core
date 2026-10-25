// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/page_script/nexus_page_script_bridge_adapter.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/page_script/page_script_options.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"

namespace nexus {

namespace errors = ::nexus::generated::page_script_errors;

namespace {

base::DictValue ReasonParams(std::string_view reason) {
  base::DictValue params;
  params.Set("reason", reason);
  return params;
}

base::DictValue OkResult(bool ok) {
  base::DictValue result;
  result.Set("ok", ok);
  return result;
}

}  // namespace

NexusPageScriptBridgeAdapter::NexusPageScriptBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile,
    WebWindowResolver resolve_web_window)
    : ::nexus::generated::PageScriptApiBase(delegate),
      profile_(profile),
      resolve_web_window_(std::move(resolve_web_window)) {}

NexusPageScriptBridgeAdapter::~NexusPageScriptBridgeAdapter() = default;

void NexusPageScriptBridgeAdapter::HandleAttachOnUI(
    ::nexus::generated::PageScriptAttachInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PageScriptTargetRequest target{input.webWindowId, input.tabId};
  if (std::optional<std::string> reason = ValidatePageScriptTarget(target)) {
    std::move(done).Error(errors::kInvalidArgs, *reason, ReasonParams(*reason));
    return;
  }
  content::WebContents* web_contents =
      input.webWindowId ? resolve_web_window_.Run(*input.webWindowId)
                        : ResolveTab(*input.tabId);
  if (!web_contents) {
    std::move(done).Error(errors::kTargetNotFound,
                          "no such window or tab on this connection");
    return;
  }
  std::unique_ptr<PageScriptChannel> channel = PageScriptChannel::Attach(
      web_contents, input.exposeToPage.value_or(false), this);
  if (!channel) {
    std::move(done).Error(errors::kAttachRefused,
                          "the page refused a script session");
    return;
  }
  const int32_t channel_id = next_channel_id_++;
  channels_[channel_id] = std::move(channel);
  base::DictValue result;
  result.Set("channelId", channel_id);
  std::move(done).Success(std::move(result));
}

void NexusPageScriptBridgeAdapter::HandleDetachOnUI(
    ::nexus::generated::PageScriptDetachInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = channels_.find(input.channelId);
  if (it == channels_.end()) {
    std::move(done).Error(errors::kChannelNotFound,
                          "no such channel on this connection");
    return;
  }
  channels_.erase(it);
  std::move(done).Success(OkResult(true));
}

void NexusPageScriptBridgeAdapter::HandleExecuteScriptOnUI(
    ::nexus::generated::PageScriptExecuteScriptInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::optional<PageScriptWorld> world = ParsePageScriptWorld(input.world);
  if (!world) {
    std::move(done).Error(errors::kInvalidArgs, "unknown world",
                          ReasonParams("unknown world"));
    return;
  }
  PageScriptChannel* channel = FindOrReject(input.channelId, done);
  if (!channel) {
    return;
  }
  channel->ExecuteScript(input.code, *world,
                         base::BindOnce(&Answer, "value", std::move(done)));
}

void NexusPageScriptBridgeAdapter::HandlePostMessageOnUI(
    ::nexus::generated::PageScriptPostMessageInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!IsPageScriptPayloadWithinLimit(input.data)) {
    std::move(done).Error(errors::kPayloadTooLarge,
                          "the message is above the size limit");
    return;
  }
  PageScriptChannel* channel = FindOrReject(input.channelId, done);
  if (!channel) {
    return;
  }
  channel->PostMessage(
      input.data,
      base::BindOnce(
          [](::nexus::BridgeResponseCallback done,
             PageScriptChannel::Outcome outcome) {
            // A page with no bootstrap yet has nobody listening; that is a
            // missing document, not a delivery.
            if (outcome.error_code.empty() && !outcome.exception &&
                outcome.value.is_int() && outcome.value.GetInt() < 0) {
              std::move(done).Error(errors::kNoDocument,
                                    "the page has no document yet");
              return;
            }
            if (outcome.error_code.empty() && !outcome.exception) {
              std::move(done).Success(OkResult(true));
              return;
            }
            Answer("ok", std::move(done), std::move(outcome));
          },
          std::move(done)));
}

void NexusPageScriptBridgeAdapter::HandleAddInitScriptOnUI(
    ::nexus::generated::PageScriptAddInitScriptInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!IsPageScriptPayloadWithinLimit(input.source)) {
    std::move(done).Error(errors::kPayloadTooLarge,
                          "the script is above the size limit");
    return;
  }
  PageScriptChannel* channel = FindOrReject(input.channelId, done);
  if (!channel) {
    return;
  }
  channel->AddInitScript(input.source,
                         base::BindOnce(&Answer, "scriptId", std::move(done)));
}

void NexusPageScriptBridgeAdapter::HandleRemoveInitScriptOnUI(
    ::nexus::generated::PageScriptRemoveInitScriptInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PageScriptChannel* channel = FindOrReject(input.channelId, done);
  if (!channel) {
    return;
  }
  channel->RemoveInitScript(input.scriptId,
                            base::BindOnce(&Answer, "ok", std::move(done)));
}

void NexusPageScriptBridgeAdapter::OnPageScriptMessage(
    PageScriptChannel* channel,
    base::DictValue payload) {
  payload.Set("channelId", IdOf(channel));
  PushEvent("onMessage", std::move(payload));
}

void NexusPageScriptBridgeAdapter::OnPageScriptDetached(
    PageScriptChannel* channel) {
  const int32_t channel_id = IdOf(channel);
  base::DictValue payload;
  payload.Set("channelId", channel_id);
  payload.Set("reason", "target_gone");
  PushEvent("onDetached", std::move(payload));
  // The channel is on the stack: drop it on the next turn.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NexusPageScriptBridgeAdapter> self, int32_t id) {
            if (self) {
              self->channels_.erase(id);
            }
          },
          weak_factory_.GetWeakPtr(), channel_id));
}

content::WebContents* NexusPageScriptBridgeAdapter::ResolveTab(
    int32_t tab_id) const {
  tabs::TabInterface* tab = tabs::TabHandle(tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return nullptr;
  }
  BrowserWindowInterface* nexus =
      custom_browser::window_util::FindNexusWindow(profile_);
  TabStripModel* strip = nexus ? nexus->tab_strip_model() : nullptr;
  if (!strip || strip->GetIndexOfWebContents(contents) == TabStripModel::kNoTab) {
    return nullptr;
  }
  return contents;
}

PageScriptChannel* NexusPageScriptBridgeAdapter::FindOrReject(
    int32_t channel_id,
    ::nexus::BridgeResponseCallback& done) {
  auto it = channels_.find(channel_id);
  if (it == channels_.end()) {
    std::move(done).Error(errors::kChannelNotFound,
                          "no such channel on this connection");
    return nullptr;
  }
  return it->second.get();
}

int32_t NexusPageScriptBridgeAdapter::IdOf(
    const PageScriptChannel* channel) const {
  for (const auto& [id, owned] : channels_) {
    if (owned.get() == channel) {
      return id;
    }
  }
  return 0;
}

// static
void NexusPageScriptBridgeAdapter::Answer(std::string_view key,
                                          ::nexus::BridgeResponseCallback done,
                                          PageScriptChannel::Outcome outcome) {
  if (!outcome.error_code.empty()) {
    std::move(done).Error(std::move(outcome.error_code),
                          "the page did not complete the request",
                          std::move(outcome.error_params));
    return;
  }
  base::DictValue result;
  if (outcome.exception) {
    result.Set("exception", *outcome.exception);
  } else if (!outcome.value.is_none()) {
    result.Set(key, std::move(outcome.value));
  }
  std::move(done).Success(std::move(result));
}

void NexusPageScriptBridgeAdapter::PushEvent(std::string_view action,
                                             base::DictValue payload) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "pageScript");
  route.Set("action", action);
  SendEvent(route, base::Value(std::move(payload)));
}

}  // namespace nexus
