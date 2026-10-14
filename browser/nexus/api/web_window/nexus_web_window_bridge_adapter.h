// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_NEXUS_WEB_WINDOW_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_NEXUS_WEB_WINDOW_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Browser;
class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// The `webWindow` capability (F-29): single-web windows — one tab, pinned to
// the kSingleWeb UI mode, never session-restored.
//
// Contract: every window belongs to this adapter, i.e. to the one bridge
// connection that opened it. Its events (`onNavigated`, `onTitleChanged`,
// `onClosed`) go to that connection only; another connection's windowId is
// `window_not_found`; destroying the adapter closes every window it owns. A
// windowId is the Browser's session id. UI thread only.
class NexusWebWindowBridgeAdapter
    : public ::nexus::generated::WebWindowApiBase {
 public:
  NexusWebWindowBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                              Profile* profile);
  ~NexusWebWindowBridgeAdapter() override;

  NexusWebWindowBridgeAdapter(const NexusWebWindowBridgeAdapter&) = delete;
  NexusWebWindowBridgeAdapter& operator=(const NexusWebWindowBridgeAdapter&) =
      delete;

 private:
  class Window;

  void HandleOpenOnUI(::nexus::generated::WebWindowOpenInput input,
                      std::vector<blink::MessagePortDescriptor> ports,
                      ::nexus::BridgeResponseCallback done) override;
  void HandleCloseOnUI(::nexus::generated::WebWindowCloseInput input,
                       std::vector<blink::MessagePortDescriptor> ports,
                       ::nexus::BridgeResponseCallback done) override;
  void HandleListOnUI(::nexus::generated::WebWindowListInput input,
                      std::vector<blink::MessagePortDescriptor> ports,
                      ::nexus::BridgeResponseCallback done) override;
  void HandleFocusOnUI(::nexus::generated::WebWindowFocusInput input,
                       std::vector<blink::MessagePortDescriptor> ports,
                       ::nexus::BridgeResponseCallback done) override;
  void HandleSetBoundsOnUI(::nexus::generated::WebWindowSetBoundsInput input,
                           std::vector<blink::MessagePortDescriptor> ports,
                           ::nexus::BridgeResponseCallback done) override;
  void HandleSetStateOnUI(::nexus::generated::WebWindowSetStateInput input,
                          std::vector<blink::MessagePortDescriptor> ports,
                          ::nexus::BridgeResponseCallback done) override;
  void HandleNavigateOnUI(::nexus::generated::WebWindowNavigateInput input,
                          std::vector<blink::MessagePortDescriptor> ports,
                          ::nexus::BridgeResponseCallback done) override;
  void HandleBackOnUI(::nexus::generated::WebWindowBackInput input,
                      std::vector<blink::MessagePortDescriptor> ports,
                      ::nexus::BridgeResponseCallback done) override;
  void HandleForwardOnUI(::nexus::generated::WebWindowForwardInput input,
                         std::vector<blink::MessagePortDescriptor> ports,
                         ::nexus::BridgeResponseCallback done) override;
  void HandleReloadOnUI(::nexus::generated::WebWindowReloadInput input,
                        std::vector<blink::MessagePortDescriptor> ports,
                        ::nexus::BridgeResponseCallback done) override;

  // The owned window `window_id`, or null after answering `done` with
  // `window_not_found` (in which case `done` has been consumed).
  Window* FindOrReject(int32_t window_id,
                       ::nexus::BridgeResponseCallback& done);

  // Called by a Window; each sends one `webWindow.<action>` event.
  void OnWindowNavigated(int32_t window_id, base::DictValue payload);
  void OnWindowTitleChanged(int32_t window_id, base::DictValue payload);
  void OnWindowClosed(int32_t window_id, std::string_view reason);

  void PushEvent(std::string_view action, base::DictValue payload);

  raw_ptr<Profile> profile_;
  std::map<int32_t, std::unique_ptr<Window>> windows_;
  base::WeakPtrFactory<NexusWebWindowBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_NEXUS_WEB_WINDOW_BRIDGE_ADAPTER_H_
