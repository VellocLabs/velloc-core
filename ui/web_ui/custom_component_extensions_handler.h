#ifndef CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_COMPONENT_EXTENSIONS_HANDLER_H_
#define CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_COMPONENT_EXTENSIONS_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace custom_browser {

class CustomComponentExtensionsHandler : public content::WebUIMessageHandler {
 public:
  explicit CustomComponentExtensionsHandler(Profile* profile);
  CustomComponentExtensionsHandler(const CustomComponentExtensionsHandler&) =
      delete;
  CustomComponentExtensionsHandler& operator=(
      const CustomComponentExtensionsHandler&) = delete;
  ~CustomComponentExtensionsHandler() override;

  void RegisterMessages() override;

 private:
  void HandleGetCustomComponentExtensions(const base::Value::List& args);
  void HandleSetCustomComponentExtensionEnabled(const base::Value::List& args);

  raw_ptr<Profile> profile_;
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_COMPONENT_EXTENSIONS_HANDLER_H_
