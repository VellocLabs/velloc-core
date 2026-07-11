#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NTP_HOOKS_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NTP_HOOKS_H_

#include <string_view>

namespace content {
class BrowserContext;
class WebUI;
class WebUIDataSource;
}  // namespace content

namespace custom_browser::patch_webui {

void OnNewTabPageDataSourceConfiguring(content::WebUI* web_ui,
                                       content::WebUIDataSource* data_source);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NTP_HOOKS_H_
