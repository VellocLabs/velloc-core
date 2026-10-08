#include "custom_browser/browser/nexus/api/browser/nexus_browser_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/tools/tool_session_registry.h"

namespace nexus {

NexusBrowserBridgeAdapter::NexusBrowserBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    ToolSessionRegistry* sessions)
    : ::nexus::generated::BrowserApiBase(delegate), sessions_(sessions) {}

NexusBrowserBridgeAdapter::~NexusBrowserBridgeAdapter() = default;

void NexusBrowserBridgeAdapter::HandleActivateTabOnUI(
    ::nexus::generated::BrowserActivateTabInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "activateTab", "BrowserActivateTab", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserActivateTabInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunActivateTabOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleBackOnUI(
    ::nexus::generated::BrowserBackInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "back", "BrowserBack", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserBackInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunBackOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleClickOnUI(
    ::nexus::generated::BrowserClickInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "click", "BrowserClick", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserClickInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunClickOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleCloseTabOnUI(
    ::nexus::generated::BrowserCloseTabInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "closeTab", "BrowserCloseTab", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserCloseTabInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunCloseTabOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleDoneOnUI(
    ::nexus::generated::BrowserDoneInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "done", "Done", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserDoneInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunDoneOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleDownloadOnUI(
    ::nexus::generated::BrowserDownloadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "download", "BrowserDownload", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserDownloadInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunDownloadOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleDragAndDropOnUI(
    ::nexus::generated::BrowserDragAndDropInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "dragAndDrop", "BrowserDragAndDrop", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserDragAndDropInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunDragAndDropOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleEvaluateOnUI(
    ::nexus::generated::BrowserEvaluateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "evaluate", "BrowserEvaluate", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserEvaluateInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunEvaluateOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleFillFormOnUI(
    ::nexus::generated::BrowserFillFormInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "fillForm", "BrowserFillForm", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserFillFormInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunFillFormOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleFindTextOnUI(
    ::nexus::generated::BrowserFindTextInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "findText", "BrowserFindText", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserFindTextInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunFindTextOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleHoverOnUI(
    ::nexus::generated::BrowserHoverInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "hover", "BrowserHover", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserHoverInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunHoverOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleInputOnUI(
    ::nexus::generated::BrowserInputInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "input", "BrowserType", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserInputInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunInputOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleMediaControlOnUI(
    ::nexus::generated::BrowserMediaControlInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "mediaControl", "BrowserMediaControl", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserMediaControlInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunMediaControlOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleMediaStateOnUI(
    ::nexus::generated::BrowserMediaStateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "mediaState", "BrowserMediaState", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserMediaStateInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunMediaStateOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleNavigateOnUI(
    ::nexus::generated::BrowserNavigateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "navigate", "BrowserNavigate", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserNavigateInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunNavigateOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleReadElementOnUI(
    ::nexus::generated::BrowserReadElementInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "readElement", "BrowserReadElement", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserReadElementInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunReadElementOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleReadPageOnUI(
    ::nexus::generated::BrowserReadPageInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "readPage", "BrowserReadPage", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserReadPageInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunReadPageOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleScreenshotOnUI(
    ::nexus::generated::BrowserScreenshotInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "screenshot", "BrowserScreenshot", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserScreenshotInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunScreenshotOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleScrollOnUI(
    ::nexus::generated::BrowserScrollInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "scroll", "BrowserScroll", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserScrollInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunScrollOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleScrollToOnUI(
    ::nexus::generated::BrowserScrollToInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "scrollTo", "BrowserScrollToElement", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserScrollToInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunScrollToOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleSearchOnUI(
    ::nexus::generated::BrowserSearchInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "search", "BrowserSearch", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserSearchInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunSearchOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleSelectOnUI(
    ::nexus::generated::BrowserSelectInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "select", "BrowserSelectOption", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserSelectInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunSelectOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

void NexusBrowserBridgeAdapter::HandleUploadOnUI(
    ::nexus::generated::BrowserUploadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  sessions_->Run(
      *this, "browser", "upload", "BrowserUpload", std::move(done),
      base::BindOnce(
          [](::nexus::generated::BrowserUploadInput input, ToolSession& session,
             ToolCallback callback) {
            session.browser_tool().RunUploadOnUI(std::move(input),
                                              std::move(callback));
          },
          std::move(input)));
}

}  // namespace nexus
