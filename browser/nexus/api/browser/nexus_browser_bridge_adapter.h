#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_BROWSER_NEXUS_BROWSER_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_BROWSER_NEXUS_BROWSER_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/browser/browser_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

namespace nexus {

class NexusBridgeResponseDelegate;
class ToolSessionRegistry;

// `browser` capability — the host actions behind the browser tools
// (navigate, click, screenshot, …). Each action is a thin forward into the
// session's BrowserTool, which drives the actor framework on UI; the
// model-facing tool lives in the runtime. Declarations:
// api/declarations/browser/*.api.json.
class NexusBrowserBridgeAdapter : public ::nexus::generated::BrowserApiBase {
 public:
  NexusBrowserBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            ToolSessionRegistry* sessions);
  ~NexusBrowserBridgeAdapter() override;

  NexusBrowserBridgeAdapter(const NexusBrowserBridgeAdapter&) = delete;
  NexusBrowserBridgeAdapter& operator=(const NexusBrowserBridgeAdapter&) =
      delete;

 private:
  void HandleActivateTabOnUI(
      ::nexus::generated::BrowserActivateTabInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleBackOnUI(
      ::nexus::generated::BrowserBackInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleClickOnUI(
      ::nexus::generated::BrowserClickInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleCloseTabOnUI(
      ::nexus::generated::BrowserCloseTabInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleDoneOnUI(
      ::nexus::generated::BrowserDoneInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleDownloadOnUI(
      ::nexus::generated::BrowserDownloadInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleDragAndDropOnUI(
      ::nexus::generated::BrowserDragAndDropInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleEvaluateOnUI(
      ::nexus::generated::BrowserEvaluateInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleFillFormOnUI(
      ::nexus::generated::BrowserFillFormInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleFindTextOnUI(
      ::nexus::generated::BrowserFindTextInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleHoverOnUI(
      ::nexus::generated::BrowserHoverInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleInputOnUI(
      ::nexus::generated::BrowserInputInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleMediaControlOnUI(
      ::nexus::generated::BrowserMediaControlInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleMediaStateOnUI(
      ::nexus::generated::BrowserMediaStateInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleNavigateOnUI(
      ::nexus::generated::BrowserNavigateInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleReadElementOnUI(
      ::nexus::generated::BrowserReadElementInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleReadPageOnUI(
      ::nexus::generated::BrowserReadPageInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleScreenshotOnUI(
      ::nexus::generated::BrowserScreenshotInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleScrollOnUI(
      ::nexus::generated::BrowserScrollInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleScrollToOnUI(
      ::nexus::generated::BrowserScrollToInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleSearchOnUI(
      ::nexus::generated::BrowserSearchInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleSelectOnUI(
      ::nexus::generated::BrowserSelectInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleUploadOnUI(
      ::nexus::generated::BrowserUploadInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<ToolSessionRegistry> sessions_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_BROWSER_NEXUS_BROWSER_BRIDGE_ADAPTER_H_
