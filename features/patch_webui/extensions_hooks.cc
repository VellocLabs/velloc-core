#include "custom_browser/features/patch_webui/extensions_hooks.h"

#include <string_view>

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_data_source.h"
#include "custom_browser/features/patch_webui/patch_resources.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"

namespace custom_browser::patch_webui {

namespace {

void AddBuiltinExtensionsStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString("customBrowserBuiltinExtensionTitle",
                                  IDS_CUSTOM_BROWSER_BUILTIN_EXTENSION_TITLE);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsName",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_NAME);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsDescription",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_DESCRIPTION);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsVersion",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_VERSION);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsId",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_ID);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsEnabled",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_ENABLED);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsDefault",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_DEFAULT);
  data_source->AddLocalizedString("customBrowserBuiltinDetailsBackgroundPage",
                                  IDS_CUSTOM_BROWSER_BUILTIN_DETAILS_BACKGROUND_PAGE);
  data_source->AddLocalizedString("customBrowserValueOn",
                                  IDS_CUSTOM_BROWSER_VALUE_ON);
  data_source->AddLocalizedString("customBrowserValueOff",
                                  IDS_CUSTOM_BROWSER_VALUE_OFF);
  data_source->AddLocalizedString("customBrowserValueYes",
                                  IDS_CUSTOM_BROWSER_VALUE_YES);
  data_source->AddLocalizedString("customBrowserValueNo",
                                  IDS_CUSTOM_BROWSER_VALUE_NO);
  data_source->AddLocalizedString("customBrowserValueNa",
                                  IDS_CUSTOM_BROWSER_VALUE_NA);
  data_source->AddLocalizedString("customBrowserCloseLabel",
                                  IDS_CUSTOM_BROWSER_CLOSE);
}

}  // namespace

void OnExtensionsDataSourceConfiguring(content::WebUI* web_ui,
                                       content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUIExtensionsHost, data_source);
  AddBuiltinExtensionsStrings(data_source);
  AddCommonPatchStrings(data_source);
}

}  // namespace custom_browser::patch_webui
