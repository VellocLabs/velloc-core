// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_PAGE_SCRIPT_NEXUS_PAGE_SCRIPT_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_PAGE_SCRIPT_NEXUS_PAGE_SCRIPT_BRIDGE_ADAPTER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/page_script/page_script_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/tools/page_script/page_script_channel.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace content {
class WebContents;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// The `pageScript` capability (F-29): script channels to pages this
// connection may reach — a single-web window it opened, or a tab in this
// profile's Nexus window.
//
// Contract: a channel belongs to this adapter, i.e. to the one bridge
// connection that attached it; its events (`onMessage`, `onDetached`) go
// to that connection only, and another connection's channelId answers
// `channel_not_found`. Destroying the adapter ends every channel without
// events; a request still open answers `no_document`, a reply that is posted
// and then dropped, since it reaches the connection through a weak handle
// the adapter's destruction has invalidated. UI thread only.
//
// Security: `webWindowId` resolves through the connection's own webWindow
// adapter, so only its windows are reachable; `tabId` must name a tab that
// sits in the profile's Nexus window — any other tab is `target_not_found`.
class NexusPageScriptBridgeAdapter
    : public ::nexus::generated::PageScriptApiBase,
      public PageScriptChannel::Delegate {
 public:
  // Resolves one of this connection's single-web windows to its page, or
  // null for an id it does not own.
  using WebWindowResolver =
      base::RepeatingCallback<content::WebContents*(int32_t window_id)>;

  NexusPageScriptBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                               Profile* profile,
                               WebWindowResolver resolve_web_window);
  ~NexusPageScriptBridgeAdapter() override;

  NexusPageScriptBridgeAdapter(const NexusPageScriptBridgeAdapter&) = delete;
  NexusPageScriptBridgeAdapter& operator=(const NexusPageScriptBridgeAdapter&) =
      delete;

 private:
  void HandleAttachOnUI(::nexus::generated::PageScriptAttachInput input,
                        std::vector<blink::MessagePortDescriptor> ports,
                        ::nexus::BridgeResponseCallback done) override;
  void HandleDetachOnUI(::nexus::generated::PageScriptDetachInput input,
                        std::vector<blink::MessagePortDescriptor> ports,
                        ::nexus::BridgeResponseCallback done) override;
  void HandleExecuteScriptOnUI(
      ::nexus::generated::PageScriptExecuteScriptInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePostMessageOnUI(
      ::nexus::generated::PageScriptPostMessageInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleAddInitScriptOnUI(
      ::nexus::generated::PageScriptAddInitScriptInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRemoveInitScriptOnUI(
      ::nexus::generated::PageScriptRemoveInitScriptInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // PageScriptChannel::Delegate:
  void OnPageScriptMessage(PageScriptChannel* channel,
                           base::DictValue payload) override;
  void OnPageScriptDetached(PageScriptChannel* channel) override;

  // The page a tab id names, when the tab sits in the profile's Nexus window.
  content::WebContents* ResolveTab(int32_t tab_id) const;

  // The channel `channel_id`, or null after answering `done` with
  // `channel_not_found` (in which case `done` has been consumed).
  PageScriptChannel* FindOrReject(int32_t channel_id,
                                  ::nexus::BridgeResponseCallback& done);
  int32_t IdOf(const PageScriptChannel* channel) const;

  // Answers `done` from a channel outcome: the value under `key`, the
  // exception, or the error.
  static void Answer(std::string_view key,
                     ::nexus::BridgeResponseCallback done,
                     PageScriptChannel::Outcome outcome);

  void PushEvent(std::string_view action, base::DictValue payload);

  raw_ptr<Profile> profile_;
  WebWindowResolver resolve_web_window_;
  int32_t next_channel_id_ = 1;
  std::map<int32_t, std::unique_ptr<PageScriptChannel>> channels_;
  base::WeakPtrFactory<NexusPageScriptBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_PAGE_SCRIPT_NEXUS_PAGE_SCRIPT_BRIDGE_ADAPTER_H_
