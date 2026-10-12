#include "custom_browser/features/patch_webui/settings_hooks.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/debug/debugging_buildflags.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/chrome_unscaled_resources.h"
#include "chrome/grit/generated_resources.h"
#include "components/strings/grit/components_strings.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/web_ui_data_source.h"
#include "custom_browser/browser/nexus/nexus_webui_config.h"
#include "custom_browser/common/branding_constants.h"
#include "custom_browser/common/product_version.h"
#include "custom_browser/features/patch_webui/patch_resources.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace custom_browser::patch_webui {

namespace {

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
  data_source->AddLocalizedString("customBrowserGetStartedLogoAlt",
                                  IDS_CUSTOM_BROWSER_LOGO_ALT);
}

void AddSettingsNexusStrings(content::WebUIDataSource* data_source) {
  // Several IDS_NEXUS_* strings contain the centralized PRODUCT_NAME or
  // PRODUCT_LONG_NAME placeholder (see custom_browser/common/branding_constants.h).
  // Substitute them via GetStringFUTF16 + AddString so loadTimeData receives
  // resolved strings; non-placeholder IDS_* keep using AddLocalizedString.
  const std::u16string product_name =
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName);
  const std::u16string product_long_name =
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductLongName);
  data_source->AddString(
      "customBrowserNexusMenuLabel",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_TITLE, product_name));
  data_source->AddString(
      "customBrowserNexusSectionTitle",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_TITLE, product_name));
  data_source->AddLocalizedString("customBrowserNexusSectionDescriptionLine0",
                                  IDS_NEXUS_SETTINGS_SECTION_DESCRIPTION_LINE_0);
  data_source->AddString(
      "customBrowserNexusSectionDescriptionLine1",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_SECTION_DESCRIPTION_LINE_1,
                                 product_name));
  data_source->AddLocalizedString("customBrowserNexusSectionDescriptionLine2",
                                  IDS_NEXUS_SETTINGS_SECTION_DESCRIPTION_LINE_2);
  data_source->AddLocalizedString("customBrowserNexusProviderPageTitle",
                                  IDS_NEXUS_SETTINGS_PROVIDER_PAGE_TITLE);
  data_source->AddLocalizedString("customBrowserNexusProviderLabel",
                                  IDS_NEXUS_SETTINGS_PROVIDER_LABEL);
  data_source->AddString(
      "customBrowserNexusProviderDescription",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_PROVIDER_DESCRIPTION,
                                 product_name));
  data_source->AddString(
      "customBrowserNexusProviderRowSubLabel",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_PROVIDER_ROW_SUBLABEL,
                                 product_name));
  data_source->AddLocalizedString("customBrowserNexusApiKeyLabel",
                                  IDS_NEXUS_SETTINGS_API_KEY_LABEL);
  data_source->AddLocalizedString("customBrowserNexusApiKeyPlaceholder",
                                  IDS_NEXUS_SETTINGS_API_KEY_PLACEHOLDER);
  data_source->AddLocalizedString("customBrowserNexusApiKeyHint",
                                  IDS_NEXUS_SETTINGS_API_KEY_HINT);
  data_source->AddLocalizedString("customBrowserNexusTestConnection",
                                  IDS_NEXUS_SETTINGS_TEST_CONNECTION);
  data_source->AddLocalizedString("customBrowserNexusConnectionSuccess",
                                  IDS_NEXUS_SETTINGS_CONNECTION_SUCCESS);
  data_source->AddLocalizedString("customBrowserNexusConnectionFailed",
                                  IDS_NEXUS_SETTINGS_CONNECTION_FAILED);
  data_source->AddLocalizedString("customBrowserNexusConnectionLabel",
                                  IDS_NEXUS_SETTINGS_CONNECTION_LABEL);
  data_source->AddLocalizedString("customBrowserNexusConnectionTestLabel",
                                  IDS_NEXUS_SETTINGS_CONNECTION_TEST_LABEL);
  data_source->AddLocalizedString("customBrowserNexusValidateTitle",
                                  IDS_NEXUS_SETTINGS_VALIDATE_TITLE);
  data_source->AddLocalizedString("customBrowserNexusValidateDescription",
                                  IDS_NEXUS_SETTINGS_VALIDATE_DESCRIPTION);
  data_source->AddLocalizedString("customBrowserNexusProviderStatusLabel",
                                  IDS_NEXUS_SETTINGS_PROVIDER_STATUS_LABEL);
  data_source->AddLocalizedString("customBrowserNexusConfigured",
                                  IDS_NEXUS_SETTINGS_PROVIDER_CONFIGURED);
  data_source->AddLocalizedString("customBrowserNexusNotConfigured",
                                  IDS_NEXUS_SETTINGS_PROVIDER_NOT_CONFIGURED);
  data_source->AddLocalizedString("customBrowserNexusProviderReady",
                                  IDS_NEXUS_SETTINGS_PROVIDER_READY);
  data_source->AddLocalizedString("customBrowserNexusProviderSetupRequired",
                                  IDS_NEXUS_SETTINGS_PROVIDER_SETUP_REQUIRED);
  data_source->AddLocalizedString("customBrowserNexusNoProviders",
                                  IDS_NEXUS_SETTINGS_NO_PROVIDERS);
  data_source->AddString(
      "customBrowserNexusNoProviderState",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_NO_PROVIDER_STATE,
                                 product_long_name));
  data_source->AddLocalizedString("customBrowserNexusModelLoading",
                                  IDS_NEXUS_SETTINGS_MODEL_LOADING);
  data_source->AddLocalizedString("customBrowserNexusModelLoadFailed",
                                  IDS_NEXUS_SETTINGS_MODEL_LOAD_FAILED);
  data_source->AddLocalizedString("customBrowserNexusNoModels",
                                  IDS_NEXUS_SETTINGS_NO_MODELS);
  data_source->AddLocalizedString("customBrowserNexusOllamaModelHint",
                                  IDS_NEXUS_SETTINGS_OLLAMA_MODEL_HINT);
  data_source->AddLocalizedString("customBrowserNexusAutomaticModelHint",
                                  IDS_NEXUS_SETTINGS_AUTOMATIC_MODEL_HINT);
  data_source->AddLocalizedString("customBrowserNexusSaveProviderModelHint",
                                  IDS_NEXUS_SETTINGS_SAVE_PROVIDER_MODEL_HINT);
  data_source->AddLocalizedString("customBrowserNexusTestingConnection",
                                  IDS_NEXUS_SETTINGS_TESTING_CONNECTION);
  data_source->AddLocalizedString("customBrowserNexusSavingProvider",
                                  IDS_NEXUS_SETTINGS_SAVING_PROVIDER);
  data_source->AddLocalizedString("customBrowserNexusProviderUpdateFailed",
                                  IDS_NEXUS_SETTINGS_UPDATE_FAILED);
  data_source->AddString(
      "customBrowserNexusWebappUrlLabel",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_WEBAPP_URL_LABEL,
                                 product_name));
  data_source->AddLocalizedString("customBrowserNexusWebappUrlPlaceholder",
                                  IDS_NEXUS_SETTINGS_WEBAPP_URL_PLACEHOLDER);
  data_source->AddString("customBrowserNexusWebappDefaultUrl",
                         nexus::kDefaultNexusGuestURL);
  data_source->AddLocalizedString("customBrowserNexusWebappUrlReset",
                                  IDS_NEXUS_SETTINGS_WEBAPP_URL_RESET);
  data_source->AddLocalizedString("customBrowserNexusWebappPageTitle",
                                  IDS_NEXUS_SETTINGS_WEBAPP_PAGE_TITLE);
  data_source->AddString(
      "customBrowserNexusWebappRowSubLabel",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_WEBAPP_ROW_SUBLABEL,
                                 product_long_name));
  data_source->AddString(
      "customBrowserNexusWebappDescription",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_WEBAPP_DESCRIPTION,
                                 product_name));
  data_source->AddLocalizedString("customBrowserNexusWebappDefaultTitle",
                                  IDS_NEXUS_SETTINGS_WEBAPP_DEFAULT_TITLE);
  data_source->AddString(
      "customBrowserNexusWebappDefaultDescription",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_WEBAPP_DEFAULT_DESCRIPTION,
                                 product_name));
  data_source->AddLocalizedString("customBrowserNexusDefaultUrlPrefix",
                                  IDS_NEXUS_SETTINGS_DEFAULT_URL_PREFIX);
  data_source->AddLocalizedString("customBrowserNexusInvalidWebappUrl",
                                  IDS_NEXUS_SETTINGS_INVALID_WEBAPP_URL);
  data_source->AddLocalizedString("customBrowserNexusInvalidWebappUrlFormat",
                                  IDS_NEXUS_SETTINGS_INVALID_WEBAPP_URL_FORMAT);
  data_source->AddLocalizedString("customBrowserNexusModelLabel",
                                  IDS_NEXUS_SETTINGS_MODEL_LABEL);
  data_source->AddLocalizedString("customBrowserNexusBaseUrlLabel",
                                  IDS_NEXUS_SETTINGS_BASE_URL_LABEL);
  data_source->AddLocalizedString("customBrowserNexusBaseUrlPlaceholder",
                                  IDS_NEXUS_SETTINGS_BASE_URL_PLACEHOLDER);
  data_source->AddLocalizedString("customBrowserNexusInvalidBaseUrl",
                                  IDS_NEXUS_SETTINGS_INVALID_BASE_URL);
  data_source->AddLocalizedString("customBrowserNexusInvalidBaseUrlFormat",
                                  IDS_NEXUS_SETTINGS_INVALID_BASE_URL_FORMAT);
  data_source->AddString(
      "customBrowserNexusOllamaNote",
      l10n_util::GetStringFUTF16(IDS_NEXUS_SETTINGS_OLLAMA_NOTE,
                                 product_long_name));
  data_source->AddLocalizedString("customBrowserNexusSelectProvider",
                                  IDS_NEXUS_SETTINGS_SELECT_PROVIDER);
  data_source->AddLocalizedString("customBrowserNexusConnectionSectionTitle",
                                  IDS_NEXUS_SETTINGS_CONNECTION_SECTION_TITLE);
  data_source->AddLocalizedString("customBrowserNexusProviderEmptyState",
                                  IDS_NEXUS_SETTINGS_PROVIDER_EMPTY_STATE);
  data_source->AddLocalizedString("customBrowserNexusVerifyLoadModels",
                                  IDS_NEXUS_SETTINGS_VERIFY_LOAD_MODELS);
  data_source->AddLocalizedString("customBrowserNexusResetProvider",
                                  IDS_NEXUS_SETTINGS_RESET_PROVIDER);
  data_source->AddLocalizedString("customBrowserNexusStoredSecurely",
                                  IDS_NEXUS_SETTINGS_STORED_SECURELY);
  data_source->AddLocalizedString("customBrowserNexusKeepStoredKey",
                                  IDS_NEXUS_SETTINGS_API_KEY_KEEP_STORED);
  data_source->AddLocalizedString("customBrowserNexusApiKeyHelper",
                                  IDS_NEXUS_SETTINGS_API_KEY_HELPER);
  data_source->AddLocalizedString("customBrowserNexusStatusNotConfigured",
                                  IDS_NEXUS_SETTINGS_STATUS_NOT_CONFIGURED);
  data_source->AddLocalizedString("customBrowserNexusStatusSelectProvider",
                                  IDS_NEXUS_SETTINGS_STATUS_SELECT_PROVIDER);
  data_source->AddLocalizedString("customBrowserNexusStatusVerifying",
                                  IDS_NEXUS_SETTINGS_STATUS_VERIFYING);
  data_source->AddLocalizedString("customBrowserNexusStatusFetchingModels",
                                  IDS_NEXUS_SETTINGS_STATUS_FETCHING_MODELS);
  data_source->AddLocalizedString(
      "customBrowserNexusStatusVerificationRequired",
      IDS_NEXUS_SETTINGS_STATUS_VERIFICATION_REQUIRED);
  data_source->AddLocalizedString("customBrowserNexusStatusConfigChanged",
                                  IDS_NEXUS_SETTINGS_STATUS_CONFIG_CHANGED);
  data_source->AddLocalizedString("customBrowserNexusStatusConnecting",
                                  IDS_NEXUS_SETTINGS_STATUS_CONNECTING);
  data_source->AddLocalizedString("customBrowserNexusStatusConnected",
                                  IDS_NEXUS_SETTINGS_STATUS_CONNECTED);
  data_source->AddLocalizedString("customBrowserNexusStatusConnectionFailed",
                                  IDS_NEXUS_SETTINGS_STATUS_CONNECTION_FAILED);
  data_source->AddLocalizedString("customBrowserNexusStatusChipNotConfigured",
                                  IDS_NEXUS_SETTINGS_STATUS_CHIP_NOT_CONFIGURED);
  data_source->AddLocalizedString("customBrowserNexusStatusChipVerifying",
                                  IDS_NEXUS_SETTINGS_STATUS_CHIP_VERIFYING);
  data_source->AddLocalizedString(
      "customBrowserNexusStatusChipChangesPending",
      IDS_NEXUS_SETTINGS_STATUS_CHIP_CHANGES_PENDING);
  data_source->AddLocalizedString("customBrowserNexusStatusChipConnecting",
                                  IDS_NEXUS_SETTINGS_STATUS_CHIP_CONNECTING);
  data_source->AddLocalizedString("customBrowserNexusStatusChipConnected",
                                  IDS_NEXUS_SETTINGS_STATUS_CHIP_CONNECTED);
  data_source->AddLocalizedString("customBrowserNexusStatusChipFailed",
                                  IDS_NEXUS_SETTINGS_STATUS_CHIP_FAILED);
  data_source->AddLocalizedString(
      "customBrowserNexusStatusModelsFoundSingular",
      IDS_NEXUS_SETTINGS_STATUS_MODELS_FOUND_SINGULAR);
  data_source->AddLocalizedString("customBrowserNexusStatusModelsFoundPlural",
                                  IDS_NEXUS_SETTINGS_STATUS_MODELS_FOUND_PLURAL);
  data_source->AddLocalizedString("customBrowserNexusErrorUnauthorized",
                                  IDS_NEXUS_SETTINGS_ERROR_UNAUTHORIZED);
  data_source->AddLocalizedString("customBrowserNexusErrorForbidden",
                                  IDS_NEXUS_SETTINGS_ERROR_FORBIDDEN);
  data_source->AddLocalizedString("customBrowserNexusErrorInvalidBaseUrl",
                                  IDS_NEXUS_SETTINGS_ERROR_INVALID_BASE_URL);
  data_source->AddLocalizedString(
      "customBrowserNexusErrorEndpointUnreachable",
      IDS_NEXUS_SETTINGS_ERROR_ENDPOINT_UNREACHABLE);
  data_source->AddLocalizedString("customBrowserNexusErrorTimeout",
                                  IDS_NEXUS_SETTINGS_ERROR_TIMEOUT);
  data_source->AddLocalizedString("customBrowserNexusErrorNoModels",
                                  IDS_NEXUS_SETTINGS_ERROR_NO_MODELS);
  data_source->AddLocalizedString("customBrowserNexusErrorUnexpectedResponse",
                                  IDS_NEXUS_SETTINGS_ERROR_UNEXPECTED_RESPONSE);
  data_source->AddLocalizedString(
      "customBrowserNexusModelMetadataContextThousands",
      IDS_NEXUS_SETTINGS_MODEL_METADATA_CONTEXT_THOUSANDS);
  data_source->AddLocalizedString("customBrowserNexusModelMetadataContext",
                                  IDS_NEXUS_SETTINGS_MODEL_METADATA_CONTEXT);
  data_source->AddLocalizedString("customBrowserNexusModelMetadataTools",
                                  IDS_NEXUS_SETTINGS_MODEL_METADATA_TOOLS);
  data_source->AddLocalizedString("customBrowserNexusModelMetadataVision",
                                  IDS_NEXUS_SETTINGS_MODEL_METADATA_VISION);
  data_source->AddLocalizedString("customBrowserNexusModelValidationEnableOne",
                                  IDS_NEXUS_SETTINGS_MODEL_VALIDATION_ENABLE_ONE);
  data_source->AddLocalizedString(
      "customBrowserNexusModelValidationSelectDefault",
      IDS_NEXUS_SETTINGS_MODEL_VALIDATION_SELECT_DEFAULT);
  data_source->AddLocalizedString(
      "customBrowserNexusModelValidationSelectDefaultOnly",
      IDS_NEXUS_SETTINGS_MODEL_VALIDATION_SELECT_DEFAULT_ONLY);
  data_source->AddLocalizedString(
      "customBrowserNexusModelValidationDefaultEnabled",
      IDS_NEXUS_SETTINGS_MODEL_VALIDATION_DEFAULT_ENABLED);
  data_source->AddLocalizedString(
      "customBrowserNexusModelValidationDefaultUnavailable",
      IDS_NEXUS_SETTINGS_MODEL_VALIDATION_DEFAULT_UNAVAILABLE);
  data_source->AddLocalizedString("customBrowserNexusModelsSectionTitle",
                                  IDS_NEXUS_SETTINGS_MODELS_SECTION_TITLE);
  data_source->AddLocalizedString(
      "customBrowserNexusModelsSectionDescription",
      IDS_NEXUS_SETTINGS_MODELS_SECTION_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserNexusDefaultModelDescription",
      IDS_NEXUS_SETTINGS_DEFAULT_MODEL_DESCRIPTION);
  data_source->AddLocalizedString(
      "customBrowserNexusDefaultModelSectionLabel",
      IDS_NEXUS_SETTINGS_DEFAULT_MODEL_SECTION_LABEL);
  data_source->AddLocalizedString(
      "customBrowserNexusDefaultModelPickerPlaceholder",
      IDS_NEXUS_SETTINGS_DEFAULT_MODEL_PICKER_PLACEHOLDER);
  data_source->AddLocalizedString(
      "customBrowserNexusDefaultModelPickerAction",
      IDS_NEXUS_SETTINGS_DEFAULT_MODEL_PICKER_ACTION);
  data_source->AddLocalizedString(
      "customBrowserNexusDefaultModelPickerHelper",
      IDS_NEXUS_SETTINGS_DEFAULT_MODEL_PICKER_HELPER);
  data_source->AddLocalizedString("customBrowserNexusCloseModelPicker",
                                  IDS_NEXUS_SETTINGS_CLOSE_MODEL_PICKER);
  data_source->AddLocalizedString("customBrowserNexusFilterModelsLabel",
                                  IDS_NEXUS_SETTINGS_FILTER_MODELS_LABEL);
  data_source->AddLocalizedString(
      "customBrowserNexusFilterModelsPlaceholder",
      IDS_NEXUS_SETTINGS_FILTER_MODELS_PLACEHOLDER);
  data_source->AddLocalizedString("customBrowserNexusNoMatchingModels",
                                  IDS_NEXUS_SETTINGS_NO_MATCHING_MODELS);
  data_source->AddLocalizedString("customBrowserNexusModelSelectedBadge",
                                  IDS_NEXUS_SETTINGS_MODEL_SELECTED_BADGE);
  data_source->AddLocalizedString(
      "customBrowserNexusEnabledModelsSectionLabel",
      IDS_NEXUS_SETTINGS_ENABLED_MODELS_SECTION_LABEL);
  data_source->AddLocalizedString("customBrowserNexusEnableModelAria",
                                  IDS_NEXUS_SETTINGS_ENABLE_MODEL_ARIA);
  data_source->AddLocalizedString("customBrowserNexusDefaultModelAria",
                                  IDS_NEXUS_SETTINGS_DEFAULT_MODEL_ARIA);
  data_source->AddLocalizedString("customBrowserNexusSummarySectionTitle",
                                  IDS_NEXUS_SETTINGS_SUMMARY_SECTION_TITLE);
  data_source->AddLocalizedString("customBrowserNexusSummaryProviderLabel",
                                  IDS_NEXUS_SETTINGS_SUMMARY_PROVIDER_LABEL);
  data_source->AddLocalizedString("customBrowserNexusSummaryDiscoveredLabel",
                                  IDS_NEXUS_SETTINGS_SUMMARY_DISCOVERED_LABEL);
  data_source->AddLocalizedString("customBrowserNexusSummaryEnabledLabel",
                                  IDS_NEXUS_SETTINGS_SUMMARY_ENABLED_LABEL);
  data_source->AddLocalizedString("customBrowserNexusSummaryDefaultLabel",
                                  IDS_NEXUS_SETTINGS_SUMMARY_DEFAULT_LABEL);
  data_source->AddLocalizedString("customBrowserNexusSummaryNone",
                                  IDS_NEXUS_SETTINGS_SUMMARY_NONE);
  data_source->AddLocalizedString("customBrowserNexusSummaryNotSelected",
                                  IDS_NEXUS_SETTINGS_SUMMARY_NOT_SELECTED);
  data_source->AddLocalizedString("customBrowserNexusSave",
                                  IDS_NEXUS_SETTINGS_SAVE);
  data_source->AddLocalizedString("customBrowserNexusBaseUrlRequired",
                                  IDS_NEXUS_SETTINGS_BASE_URL_REQUIRED);
  data_source->AddLocalizedString("customBrowserNexusApiKeyRequired",
                                  IDS_NEXUS_SETTINGS_API_KEY_REQUIRED);
  data_source->AddLocalizedString("customBrowserNexusSwitchProviderConfirm",
                                  IDS_NEXUS_SETTINGS_SWITCH_PROVIDER_CONFIRM);
  data_source->AddLocalizedString("customBrowserNexusSaveFailed",
                                  IDS_NEXUS_SETTINGS_SAVE_FAILED);
  data_source->AddLocalizedString("customBrowserNexusSaveSuccess",
                                  IDS_NEXUS_SETTINGS_SAVE_SUCCESS);
  data_source->AddLocalizedString("customBrowserNexusResetConfirm",
                                  IDS_NEXUS_SETTINGS_RESET_CONFIRM);
  data_source->AddLocalizedString("customBrowserNexusResetFailed",
                                  IDS_NEXUS_SETTINGS_RESET_FAILED);
  data_source->AddLocalizedString("customBrowserNexusProviderOpenai",
                                  IDS_NEXUS_PROVIDER_OPENAI);
  data_source->AddLocalizedString("customBrowserNexusProviderAnthropic",
                                  IDS_NEXUS_PROVIDER_ANTHROPIC);
  data_source->AddLocalizedString("customBrowserNexusProviderGoogle",
                                  IDS_NEXUS_PROVIDER_GOOGLE);
  data_source->AddLocalizedString("customBrowserNexusProviderOllama",
                                  IDS_NEXUS_PROVIDER_OLLAMA);
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

}  // namespace

void OnSettingsDataSourceConfiguring(content::WebUI* web_ui,
                                     content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUISettingsHost, data_source);
  AddSettingsGetStartedStrings(data_source);
  AddSettingsNexusStrings(data_source);
  AddCustomizeChromeStrings(data_source);
  AddSettingsVersionDetailStrings(data_source);
  AddVersionStrings(data_source);
  AddCommonPatchStrings(data_source);
  // Add high-resolution product logo SVG for Get Started page
  data_source->AddResourcePath("images/product-logo.svg", IDR_PRODUCT_LOGO_SVG);
  data_source->AddLocalizedString("peoplePageTitle", IDS_CUSTOM_BROWSER_GET_STARTED_TITLE);
}

void OnCustomizeChromeSidePanelDataSourceConfiguring(
    content::WebUI* web_ui,
    content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUICustomizeChromeSidePanelHost, data_source);
  AddCustomizeChromeStrings(data_source);
  AddCommonPatchStrings(data_source);
}

void OnVersionDataSourceConfiguring(content::WebUI* web_ui,
                                    content::WebUIDataSource* data_source) {
  AddPatchResourcesForHost(chrome::kChromeUIVersionHost, data_source);
  AddVersionStrings(data_source);
}

}  // namespace custom_browser::patch_webui
