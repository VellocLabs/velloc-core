#include "custom_browser/features/patch_webui/ntp_hooks.h"

#include <string>
#include <string_view>

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/search_engine_type.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "custom_browser/common/pref_names.h"
#include "custom_browser/features/patch_webui/patch_resources.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"

namespace custom_browser::patch_webui {

namespace {

void AddNewTabPageData(content::BrowserContext* browser_context,
                       content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString("customNtpGetStartedPageTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_TITLE);
  data_source->AddLocalizedString("customNtpGetStartedPageBody",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_BODY);
  data_source->AddLocalizedString("customNtpGetStartedCloseLabel",
                                  IDS_CUSTOM_BROWSER_CLOSE);
  data_source->AddLocalizedString(
      "customNtpSetupListGetStartedBody",
      IDS_CUSTOM_BROWSER_NTP_SETUP_LIST_GET_STARTED_BODY);
  data_source->AddLocalizedString(
      "customNtpSetupListGetStartedAction",
      IDS_CUSTOM_BROWSER_NTP_SETUP_LIST_GET_STARTED_ACTION);

  Profile* profile =
      Profile::FromBrowserContext(browser_context);
  bool show_search = true;
  bool show_logo = true;
  bool show_shortcuts = true;
  if (profile) {
    PrefService* prefs = profile->GetPrefs();
    if (prefs) {
      show_search = prefs->GetBoolean(kCustomNewTabShowSearchPrefPath);
      show_logo = prefs->GetBoolean(kCustomNewTabShowLogoPrefPath);
      show_shortcuts = prefs->GetBoolean(kCustomNewTabShowShortcutsPrefPath);
    }
  }
  data_source->AddBoolean("customNtpShowSearch", show_search);
  data_source->AddBoolean("customNtpShowLogo", show_logo);
  data_source->AddBoolean("customNtpShowShortcuts", show_shortcuts);

  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  TemplateURLService* template_url_service =
      TemplateURLServiceFactory::GetForProfile(profile);
  if (!template_url_service) {
    return;
  }

  const TemplateURL* template_url =
      template_url_service->GetDefaultSearchProvider();
  if (!template_url) {
    return;
  }

  const auto& search_terms_data = template_url_service->search_terms_data();
  if (template_url->GetEngineType(search_terms_data) ==
      SEARCH_ENGINE_GOOGLE) {
    return;
  }

  const GURL search_url =
      template_url->GenerateSearchURL(search_terms_data, u"keyword");
  if (search_url.is_valid()) {
    data_source->AddString("customSearchEngineName",
                           base::UTF16ToUTF8(template_url->short_name()));
    data_source->AddString("customSearchEngineUrl", search_url.spec());
  }

  // NewTabPageUI
  data_source->AddBoolean("expandedSearchboxShowVoiceSearch", false);
  data_source->AddBoolean("expandedComposeboxShowVoiceSearch", false);

  return;
}

}  // namespace

void OnNewTabPageDataSourceConfiguring(content::WebUI* web_ui,
                                       content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUINewTabPageHost, data_source);
  if (web_ui && web_ui->GetWebContents()) {
    AddNewTabPageData(web_ui->GetWebContents()->GetBrowserContext(), data_source);
  }
}

}  // namespace custom_browser::patch_webui
