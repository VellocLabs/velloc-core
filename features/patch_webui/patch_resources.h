#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_PATCH_RESOURCES_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_PATCH_RESOURCES_H_

#include <string_view>

namespace content {
class WebUIDataSource;
}  // namespace content

namespace custom_browser::patch_webui {

// Adds any grit-packed resources whose path contains |host| to |data_source|.
void AddPatchResourcesForHost(std::string_view host,
                              content::WebUIDataSource* data_source);

// Adds common strings shared across multiple WebUI hosts (mode strings, pref paths).
void AddCommonPatchStrings(content::WebUIDataSource* data_source);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_PATCH_RESOURCES_H_
