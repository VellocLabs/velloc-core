#include "custom_browser/features/patch_webui/register_hooks.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/debug/debugging_buildflags.h"
#include "base/memory/weak_ptr.h"
#include "base/files/file_util.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/chrome_unscaled_resources.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/patch_webui_resources_map.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui_data_source.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/search_engine_type.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"
#include "components/strings/grit/components_strings.h"
#include "components/version_info/version_info.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/common/product_version.h"
#include "custom_browser/common/pref_names.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace custom_browser::patch_webui {

namespace {
void AddPatchResourcesForHost(std::string_view host,
                              content::WebUIDataSource* data_source) {
  if (!data_source || host.empty()) {
    return;
  }

  for (const auto& item : kPatchWebuiResources) {
    const std::string_view path(item.path);
    if (path.find(host) == std::string_view::npos) {
      continue;
    }

    const size_t last_slash = path.find_last_of('/');
    const std::string_view file_name =
        last_slash == std::string_view::npos ? path
                                             : path.substr(last_slash + 1);
    data_source->AddResourcePath(file_name, item.id);
  }
}

void AddLocalizedModeStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString("modesHeader",
                                  IDS_CUSTOM_BROWSER_MODES_HEADER);
  data_source->AddLocalizedString("modeSidebar",
                                  IDS_CUSTOM_BROWSER_MODE_SIDEBAR);
  data_source->AddLocalizedString("modeCompactSidebar",
                                  IDS_CUSTOM_BROWSER_MODE_COMPACT_SIDEBAR);
  data_source->AddLocalizedString("modeStandard",
                                  IDS_CUSTOM_BROWSER_MODE_STANDARD);
  data_source->AddLocalizedString("modeCompactStandard",
                                  IDS_CUSTOM_BROWSER_MODE_COMPACT_STANDARD);
  data_source->AddLocalizedString("modeSidebarDescription",
                                  IDS_CUSTOM_BROWSER_MODE_SIDEBAR_DESCRIPTION);
  data_source->AddLocalizedString(
      "modeCompactSidebarDescription",
      IDS_CUSTOM_BROWSER_MODE_COMPACT_SIDEBAR_DESCRIPTION);
  data_source->AddLocalizedString("modeStandardDescription",
                                  IDS_CUSTOM_BROWSER_MODE_STANDARD_DESCRIPTION);
  data_source->AddLocalizedString(
      "modeCompactStandardDescription",
      IDS_CUSTOM_BROWSER_MODE_COMPACT_STANDARD_DESCRIPTION);
}

void AddCustomizeChromeStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString(
      "customizeChromeNtpSectionTitle",
      IDS_CUSTOM_BROWSER_CUSTOMIZE_CHROME_NTP_SECTION_TITLE);
  data_source->AddLocalizedString(
      "customizeChromeNtpShowSearchLabel",
      IDS_CUSTOM_BROWSER_CUSTOMIZE_CHROME_SHOW_SEARCH);
  data_source->AddLocalizedString(
      "customizeChromeNtpShowLogoLabel",
      IDS_CUSTOM_BROWSER_CUSTOMIZE_CHROME_SHOW_LOGO);
  data_source->AddLocalizedString(
      "customizeChromeNtpSearchEngineLabel",
      IDS_CUSTOM_BROWSER_CUSTOMIZE_CHROME_SEARCH_ENGINE);
  data_source->AddLocalizedString("customBrowserLoadingLabel",
                                  IDS_CUSTOM_BROWSER_LOADING);
}

void AddSettingsGetStartedStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString("customBrowserGetStartedMenuLabel",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_MENU_LABEL);
  data_source->AddLocalizedString("customBrowserGetStartedPageTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_HERO_TITLE);
  data_source->AddLocalizedString("customBrowserGetStartedSubtitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_SUBTITLE);
  data_source->AddLocalizedString("customBrowserGetStartedBringTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_BRING_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedBringDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_BRING_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedImportButton",
      IDS_CUSTOM_BROWSER_GET_STARTED_IMPORT_BUTTON);
  data_source->AddLocalizedString("customBrowserGetStartedLayoutTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutSidebarTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_SIDEBAR_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutSidebarDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_SIDEBAR_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutCompactSidebarTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_COMPACT_SIDEBAR_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutCompactSidebarDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_COMPACT_SIDEBAR_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutTopTabsTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_TOP_TABS_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutTopTabsDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_TOP_TABS_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutCompactTopTabsTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_COMPACT_TOP_TABS_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedLayoutCompactTopTabsDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_COMPACT_TOP_TABS_DESCRIPTION);
  data_source->AddLocalizedString("customBrowserGetStartedLayoutHint",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_HINT);
  data_source->AddLocalizedString("customBrowserGetStartedLayoutAction",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_LAYOUT_ACTION);
  data_source->AddLocalizedString("customBrowserGetStartedNewTabTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_NEW_TAB_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedNewTabDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_NEW_TAB_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedNewTabToggleLabel",
      IDS_CUSTOM_BROWSER_GET_STARTED_NEW_TAB_TOGGLE_LABEL);
  data_source->AddLocalizedString(
      "customBrowserGetStartedNewTabToggleOn",
      IDS_CUSTOM_BROWSER_GET_STARTED_NEW_TAB_TOGGLE_ON);
  data_source->AddLocalizedString(
      "customBrowserGetStartedNewTabToggleOff",
      IDS_CUSTOM_BROWSER_GET_STARTED_NEW_TAB_TOGGLE_OFF);
  data_source->AddLocalizedString(
      "customBrowserGetStartedSearchBrandingTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_SEARCH_BRANDING_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedSearchBrandingDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_SEARCH_BRANDING_DESCRIPTION);
  data_source->AddLocalizedString("customBrowserGetStartedToolsTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_TOOLS_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolsDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOLS_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolUblockTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOL_UBLOCK_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolUblockStatus",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOL_UBLOCK_STATUS);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolUblockDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOL_UBLOCK_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolTamperTitle",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOL_TAMPER_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolTamperStatus",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOL_TAMPER_STATUS);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolTamperDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOL_TAMPER_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolsManageButton",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOLS_MANAGE_BUTTON);
  data_source->AddLocalizedString(
      "customBrowserGetStartedToolsFootnote",
      IDS_CUSTOM_BROWSER_GET_STARTED_TOOLS_FOOTNOTE);
  data_source->AddLocalizedString("customBrowserGetStartedFinishTitle",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_FINISH_TITLE);
  data_source->AddLocalizedString(
      "customBrowserGetStartedFinishDescription",
      IDS_CUSTOM_BROWSER_GET_STARTED_FINISH_DESCRIPTION);
  data_source->AddLocalizedString("customBrowserGetStartedFinishPrimary",
                                  IDS_CUSTOM_BROWSER_GET_STARTED_FINISH_PRIMARY);
  data_source->AddLocalizedString(
      "customBrowserGetStartedFinishSecondary",
      IDS_CUSTOM_BROWSER_GET_STARTED_FINISH_SECONDARY);
  data_source->AddLocalizedString(
      "customBrowserGetStartedFinishTertiary",
      IDS_CUSTOM_BROWSER_GET_STARTED_FINISH_TERTIARY);
  data_source->AddLocalizedString("customBrowserBadgeRecommended",
                                  IDS_CUSTOM_BROWSER_BADGE_RECOMMENDED);
  data_source->AddLocalizedString("customBrowserBadgeMinimal",
                                  IDS_CUSTOM_BROWSER_BADGE_MINIMAL);
  data_source->AddLocalizedString("customBrowserBadgeComingSoon",
                                  IDS_CUSTOM_BROWSER_BADGE_COMING_SOON);
  data_source->AddLocalizedString("customBrowserBadgeAutomatic",
                                  IDS_CUSTOM_BROWSER_BADGE_AUTOMATIC);
  data_source->AddLocalizedString("customBrowserBadgeAdvanced",
                                  IDS_CUSTOM_BROWSER_BADGE_ADVANCED);
  data_source->AddLocalizedString("customBrowserGetStartedLogoAlt",
                                  IDS_CUSTOM_BROWSER_LOGO_ALT);
}

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

void AddVersionStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString("customBrowserVersionLine",
                                  IDS_CUSTOM_BROWSER_VERSION_LINE);
  data_source->AddLocalizedString("customBrowserBasedOnChromiumLine",
                                  IDS_CUSTOM_BROWSER_BASED_ON_CHROMIUM_LINE);
  data_source->AddString("customBrowserProductVersion",
                         custom_browser::kCustomBrowserProductVersion);
  data_source->AddString("customBrowserChromiumVersion",
                         version_info::GetVersionNumber());
}

std::string GetSettingsVersionModifier() {
  std::vector<std::string> modifier_parts;
  if (std::string channel_name =
          chrome::GetChannelName(chrome::WithExtendedStable(true));
      !channel_name.empty()) {
    modifier_parts.push_back(std::move(channel_name));
  }
#if BUILDFLAG(DCHECK_IS_CONFIGURABLE)
  modifier_parts.emplace_back("dcheck");
#endif  // BUILDFLAG(DCHECK_IS_CONFIGURABLE)
  return base::JoinString(modifier_parts, "-");
}

void AddSettingsVersionDetailStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString(
      "official",
      version_info::IsOfficialBuild() ? IDS_VERSION_UI_OFFICIAL
                                      : IDS_VERSION_UI_UNOFFICIAL);
  data_source->AddString("version_modifier", GetSettingsVersionModifier());
}

void AddCommonPatchStrings(content::WebUIDataSource* data_source) {
  data_source->AddString("customBrowserUIModePrefPath", kCustomUIModePrefPath);
  data_source->AddString("customNewTabShowSearchPrefPath",
                         kCustomNewTabShowSearchPrefPath);
  data_source->AddString("customNewTabShowLogoPrefPath",
                         kCustomNewTabShowLogoPrefPath);
  data_source->AddString("customNewTabShowShortcutsPrefPath",
                         kCustomNewTabShowShortcutsPrefPath);
  AddLocalizedModeStrings(data_source);
}

int OverrideLocalizedString(content::WebUIDataSource* data_source,
                            std::string_view name,
                            int ids) {
  if (ids == IDS_SETTINGS_PEOPLE) {
    DCHECK(name == "peoplePageTitle");
    DCHECK(data_source->GetSource() == chrome::kChromeUISettingsHost);
    return IDS_CUSTOM_BROWSER_GET_STARTED_TITLE;
  }
  return ids;
}

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

  // data_source->AddString("searchBoxPlaceholder",
  //                         l10n_util::GetStringFUTF16(
  //                             IDS_WEBUI_OMNIBOX_PLACEHOLDER_TEXT, u"Google"));

  return;
}

void OnNewTabPageDataSourceCreated(content::BrowserContext* browser_context,
                                   content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUINewTabPageHost, data_source);
  AddNewTabPageData(browser_context, data_source);
}

void OnSettingsDataSourceCreated(content::BrowserContext* /*browser_context*/,
                                 content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUISettingsHost, data_source);
  AddSettingsGetStartedStrings(data_source);
  AddCustomizeChromeStrings(data_source);
  AddSettingsVersionDetailStrings(data_source);
  AddVersionStrings(data_source);
  AddCommonPatchStrings(data_source);
  // Add high-resolution product logo SVG for Get Started page
  data_source->AddResourcePath("images/product-logo.svg", IDR_PRODUCT_LOGO_SVG);
}

void OnExtensionsDataSourceCreated(content::BrowserContext* /*browser_context*/,
                                   content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUIExtensionsHost, data_source);
  AddBuiltinExtensionsStrings(data_source);
  AddCommonPatchStrings(data_source);
}

void OnCustomizeChromeSidePanelDataSourceCreated(
    content::BrowserContext* /*browser_context*/,
    content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUICustomizeChromeSidePanelHost, data_source);
  AddCustomizeChromeStrings(data_source);
  AddCommonPatchStrings(data_source);
}

void OnVersionDataSourceCreated(content::BrowserContext* /*browser_context*/,
                                content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUIVersionHost, data_source);
  AddVersionStrings(data_source);
}

void LoadCustomBrowserStrings() {
  base::FilePath assets_path;
  if (!base::PathService::Get(base::DIR_ASSETS, &assets_path)) {
    return;
  }

  auto try_load = [&](const std::string& locale) -> bool {
    if (locale.empty()) {
      return false;
    }
    const base::FilePath pak_path = assets_path.AppendASCII(
        base::StrCat({"custom_browser_strings_", locale, ".pak"}));
    if (!base::PathExists(pak_path)) {
      return false;
    }
    ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
        pak_path, ui::kScaleFactorNone);
    return true;
  };

  std::string locale =
      g_browser_process ? g_browser_process->GetApplicationLocale() : "";
  base::ReplaceChars(locale, "_", "-", &locale);

  if (try_load(locale)) {
    return;
  }

  const size_t dash = locale.find('-');
  if (dash != std::string::npos) {
    const std::string parent_locale = locale.substr(0, dash);
    if (try_load(parent_locale)) {
      return;
    }
  }

  try_load("en-US");
}

void NavigateToNewTab(base::WeakPtr<content::WebContents> web_contents,
                      GURL target_url) {
  if (!web_contents || !target_url.is_valid()) {
    return;
  }

  content::NavigationController::LoadURLParams params(target_url);
  params.transition_type = ui::PAGE_TRANSITION_FROM_ADDRESS_BAR;
  params.is_renderer_initiated = false;
  web_contents->GetController().LoadURLWithParams(params);
}

class SearchRedirectThrottle : public content::NavigationThrottle {
 public:
  explicit SearchRedirectThrottle(content::NavigationThrottleRegistry& registry)
      : content::NavigationThrottle(registry) {}
  ~SearchRedirectThrottle() override = default;

  const char* GetNameForLogging() override {
    return "CustomBrowserSearchRedirectThrottle";
  }

  ThrottleCheckResult WillStartRequest() override {
    return MaybeRedirectToNewTab();
  }

  ThrottleCheckResult WillRedirectRequest() override {
    return MaybeRedirectToNewTab();
  }

 private:
  ThrottleCheckResult MaybeRedirectToNewTab() {
    content::NavigationHandle* handle = navigation_handle();
    if (!handle->IsInMainFrame() || handle->IsSameDocument()) {
      return PROCEED;
    }

    const GURL& url = handle->GetURL();
    if (!url.SchemeIsHTTPOrHTTPS()) {
      return PROCEED;
    }

    content::WebContents* web_contents = handle->GetWebContents();
    if (!web_contents) {
      return PROCEED;
    }

    Profile* profile =
        Profile::FromBrowserContext(web_contents->GetBrowserContext());
    if (!profile || profile->IsOffTheRecord()) {
      return PROCEED;
    }

    if (search::GetNewTabPageURL(profile) != url || search::DefaultSearchProviderIsGoogle(profile)) {
      return PROCEED;
    }

    GURL target_url(chrome::kChromeUINewTabPageURL);
    base::WeakPtr<content::WebContents> weak_contents =
        web_contents->GetWeakPtr();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&NavigateToNewTab, std::move(weak_contents),
                       std::move(target_url)));
    return CANCEL_AND_IGNORE;
  }
};

void AddSearchRedirectThrottle(
    content::NavigationThrottleRegistry& registry) {
  if (!registry.IsHTTPOrHTTPS()) {
    return;
  }

  registry.AddThrottle(
      std::make_unique<SearchRedirectThrottle>(registry));
}

}  // namespace

void RegisterHooks(custom_browser::HookRegistrar* registrar) {
  LoadCustomBrowserStrings();
  registrar->AddWebUIDataSourceCreatedCallback(
      chrome::kChromeUINewTabPageHost,
      base::BindRepeating(&OnNewTabPageDataSourceCreated));
  registrar->AddWebUIDataSourceCreatedCallback(
      chrome::kChromeUISettingsHost,
      base::BindRepeating(&OnSettingsDataSourceCreated));
  registrar->AddWebUIDataSourceCreatedCallback(
      chrome::kChromeUIExtensionsHost,
      base::BindRepeating(&OnExtensionsDataSourceCreated));
  registrar->AddWebUIDataSourceCreatedCallback(
      chrome::kChromeUICustomizeChromeSidePanelHost,
      base::BindRepeating(&OnCustomizeChromeSidePanelDataSourceCreated));
  registrar->AddWebUIDataSourceCreatedCallback(
      chrome::kChromeUIVersionHost,
      base::BindRepeating(&OnVersionDataSourceCreated));
  registrar->AddLocalizedStringOverrideCallback(
      base::BindRepeating(&OverrideLocalizedString));
  registrar->AddNavigationThrottleProvider(
      base::BindRepeating(&AddSearchRedirectThrottle));
}

}  // namespace custom_browser::patch_webui
