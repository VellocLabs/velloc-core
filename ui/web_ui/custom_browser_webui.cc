
#include "custom_browser/ui/web_ui/custom_browser_webui.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_properties.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui/favicon_source.h"
#include "chrome/browser/ui/webui/tab_strip/tab_strip_ui_layout.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/custom_browser_resources.h"
#include "chrome/grit/custom_browser_resources_map.h"
#include "chrome/grit/generated_resources.h"
#include "components/favicon_base/favicon_url_parser.h"
#include "components/prefs/pref_service.h"
#include "components/strings/grit/components_strings.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "custom_browser/browser/nexus/settings/nexus_settings_handler.h"
#include "custom_browser/common/product_version.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/web_ui/custom_browser_handler.h"
#include "custom_browser/ui/web_ui/native_view_handler.h"
#include "net/base/url_util.h"
#include "ui/base/theme_provider.h"
#include "ui/base/ui_base_features.h"
#include "ui/base/webui/web_ui_util.h"
#include "ui/gfx/color_utils.h"
#include "ui/webui/color_change_listener/color_change_handler.h"
#include "ui/webui/webui_util.h"

namespace {

void AddNexusStrings(content::WebUIDataSource* html_source) {
  static constexpr webui::LocalizedString kNexusStrings[] = {
      {"customBrowserLoadingLabel", IDS_CUSTOM_BROWSER_LOADING},
      {"customBrowserNexusProviderPageTitle",
       IDS_NEXUS_SETTINGS_PROVIDER_PAGE_TITLE},
      {"customBrowserNexusProviderDescription",
       IDS_NEXUS_SETTINGS_PROVIDER_DESCRIPTION},
      {"customBrowserNexusProviderConnectionDescription",
       IDS_NEXUS_SETTINGS_PROVIDER_CONNECTION_DESCRIPTION},
      {"customBrowserNexusProviderLabel", IDS_NEXUS_SETTINGS_PROVIDER_LABEL},
      {"customBrowserNexusNoProviders", IDS_NEXUS_SETTINGS_NO_PROVIDERS},
      {"customBrowserNexusProviderOpenai", IDS_NEXUS_PROVIDER_OPENAI},
      {"customBrowserNexusProviderAnthropic", IDS_NEXUS_PROVIDER_ANTHROPIC},
      {"customBrowserNexusProviderGoogle", IDS_NEXUS_PROVIDER_GOOGLE},
      {"customBrowserNexusProviderOllama", IDS_NEXUS_PROVIDER_OLLAMA},
      {"customBrowserNexusBaseUrlLabel", IDS_NEXUS_SETTINGS_BASE_URL_LABEL},
      {"customBrowserNexusBaseUrlPlaceholder",
       IDS_NEXUS_SETTINGS_BASE_URL_PLACEHOLDER},
      {"customBrowserNexusApiKeyLabel", IDS_NEXUS_SETTINGS_API_KEY_LABEL},
      {"customBrowserNexusApiKeyPlaceholder",
       IDS_NEXUS_SETTINGS_API_KEY_PLACEHOLDER},
      {"customBrowserNexusKeepStoredKey",
       IDS_NEXUS_SETTINGS_API_KEY_KEEP_STORED},
      {"customBrowserNexusApiKeyHelper", IDS_NEXUS_SETTINGS_API_KEY_HELPER},
      {"customBrowserNexusVerifyLoadModels",
       IDS_NEXUS_SETTINGS_VERIFY_LOAD_MODELS},
      {"customBrowserNexusSave", IDS_NEXUS_SETTINGS_SAVE},
      {"customBrowserNexusUpdateFailed", IDS_NEXUS_SETTINGS_UPDATE_FAILED},
      {"customBrowserNexusResetConnection",
       IDS_NEXUS_SETTINGS_RESET_CONNECTION},
      {"customBrowserNexusResetDialogTitle",
       IDS_NEXUS_SETTINGS_RESET_DIALOG_TITLE},
      {"customBrowserNexusResetDialogBody",
       IDS_NEXUS_SETTINGS_RESET_DIALOG_BODY},
      {"customBrowserNexusResetCompleted",
       IDS_NEXUS_SETTINGS_RESET_COMPLETED},
      {"customBrowserNexusConnectionSectionTitle",
       IDS_NEXUS_SETTINGS_CONNECTION_SECTION_TITLE},
      {"customBrowserNexusModelsSectionTitle",
       IDS_NEXUS_SETTINGS_MODELS_SECTION_TITLE},
      {"customBrowserNexusModelLabel", IDS_NEXUS_SETTINGS_MODEL_LABEL},
      {"customBrowserNexusProviderReady", IDS_NEXUS_SETTINGS_PROVIDER_READY},
      {"customBrowserNexusProviderSetupRequired",
       IDS_NEXUS_SETTINGS_PROVIDER_SETUP_REQUIRED},
      {"customBrowserNexusStatusNotConfigured",
       IDS_NEXUS_SETTINGS_STATUS_NOT_CONFIGURED},
      {"customBrowserNexusStatusSelectProvider",
       IDS_NEXUS_SETTINGS_STATUS_SELECT_PROVIDER},
      {"customBrowserNexusStatusVerificationRequired",
       IDS_NEXUS_SETTINGS_STATUS_VERIFICATION_REQUIRED},
      {"customBrowserNexusStatusConfigChanged",
       IDS_NEXUS_SETTINGS_STATUS_CONFIG_CHANGED},
      {"customBrowserNexusStatusConnecting",
       IDS_NEXUS_SETTINGS_STATUS_CONNECTING},
      {"customBrowserNexusStatusFetchingModels",
       IDS_NEXUS_SETTINGS_STATUS_FETCHING_MODELS},
      {"customBrowserNexusStatusConnected",
       IDS_NEXUS_SETTINGS_STATUS_CONNECTED},
      {"customBrowserNexusStatusConnectionFailed",
       IDS_NEXUS_SETTINGS_STATUS_CONNECTION_FAILED},
      {"customBrowserNexusStatusChipNotConfigured",
       IDS_NEXUS_SETTINGS_STATUS_CHIP_NOT_CONFIGURED},
      {"customBrowserNexusStatusChipChangesPending",
       IDS_NEXUS_SETTINGS_STATUS_CHIP_CHANGES_PENDING},
      {"customBrowserNexusStatusChipConnecting",
       IDS_NEXUS_SETTINGS_STATUS_CHIP_CONNECTING},
      {"customBrowserNexusStatusChipConnected",
       IDS_NEXUS_SETTINGS_STATUS_CHIP_CONNECTED},
      {"customBrowserNexusStatusChipFailed",
       IDS_NEXUS_SETTINGS_STATUS_CHIP_FAILED},
      {"customBrowserNexusStatusModelsFoundSingular",
       IDS_NEXUS_SETTINGS_STATUS_MODELS_FOUND_SINGULAR},
      {"customBrowserNexusStatusModelsFoundPlural",
       IDS_NEXUS_SETTINGS_STATUS_MODELS_FOUND_PLURAL},
      {"customBrowserNexusBaseUrlRequired",
       IDS_NEXUS_SETTINGS_BASE_URL_REQUIRED},
      {"customBrowserNexusApiKeyRequired",
       IDS_NEXUS_SETTINGS_API_KEY_REQUIRED},
      {"customBrowserNexusModelValidationSelectDefaultOnly",
       IDS_NEXUS_SETTINGS_MODEL_VALIDATION_SELECT_DEFAULT_ONLY},
      {"customBrowserNexusSaveFailed", IDS_NEXUS_SETTINGS_SAVE_FAILED},
      {"customBrowserNexusSaveSuccess", IDS_NEXUS_SETTINGS_SAVE_SUCCESS},
      {"customBrowserNexusErrorUnauthorized",
       IDS_NEXUS_SETTINGS_ERROR_UNAUTHORIZED},
      {"customBrowserNexusErrorForbidden",
       IDS_NEXUS_SETTINGS_ERROR_FORBIDDEN},
      {"customBrowserNexusErrorInvalidBaseUrl",
       IDS_NEXUS_SETTINGS_ERROR_INVALID_BASE_URL},
      {"customBrowserNexusErrorEndpointUnreachable",
       IDS_NEXUS_SETTINGS_ERROR_ENDPOINT_UNREACHABLE},
      {"customBrowserNexusErrorTimeout", IDS_NEXUS_SETTINGS_ERROR_TIMEOUT},
      {"customBrowserNexusErrorNoModels", IDS_NEXUS_SETTINGS_ERROR_NO_MODELS},
      {"customBrowserNexusErrorUnexpectedResponse",
       IDS_NEXUS_SETTINGS_ERROR_UNEXPECTED_RESPONSE},
      {"customBrowserNexusOllamaNote", IDS_NEXUS_SETTINGS_OLLAMA_NOTE},
      {"customBrowserNexusWarningTitle", IDS_NEXUS_WARNING_TITLE},
      {"customBrowserNexusWarningIntro", IDS_NEXUS_WARNING_INTRO},
      {"customBrowserNexusWarningBody1", IDS_NEXUS_WARNING_BODY_1},
      {"customBrowserNexusWarningBody2", IDS_NEXUS_WARNING_BODY_2},
      {"customBrowserNexusWarningBody3", IDS_NEXUS_WARNING_BODY_3},
      {"customBrowserNexusWarningRecommendationsTitle",
       IDS_NEXUS_WARNING_RECOMMENDATIONS_TITLE},
      {"customBrowserNexusWarningRecommendation1",
       IDS_NEXUS_WARNING_RECOMMENDATION_ENABLE_TRUSTED},
      {"customBrowserNexusWarningRecommendation2",
       IDS_NEXUS_WARNING_RECOMMENDATION_MINIMUM_PERMISSIONS},
      {"customBrowserNexusWarningRecommendation3",
       IDS_NEXUS_WARNING_RECOMMENDATION_AVOID_SENSITIVE_DATA},
      {"customBrowserNexusWarningRecommendation4",
       IDS_NEXUS_WARNING_RECOMMENDATION_REVIEW_PROVIDERS},
      {"customBrowserNexusWarningRecommendation5",
       IDS_NEXUS_WARNING_RECOMMENDATION_USE_SAFEGUARDS},
      {"customBrowserNexusWarningSharedDevice",
       IDS_NEXUS_WARNING_SHARED_DEVICE},
      {"customBrowserNexusWarningAcknowledgment",
       IDS_NEXUS_WARNING_ACKNOWLEDGMENT},
      {"customBrowserNexusWarningContinue", IDS_NEXUS_WARNING_CONTINUE},
      {"customBrowserCancel", IDS_CANCEL},
  };
  html_source->AddLocalizedStrings(kNexusStrings);
}

}  // namespace

CustomBrowserWebUI::CustomBrowserWebUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui, /* enable_chrome_send */ true),
      webui_load_timer_(web_ui->GetWebContents(),
                        "WebUITabStrip.LoadDocumentTime",
                        "WebUITabStrip.LoadCompletedTime") {
  content::HostZoomMap::Get(web_ui->GetWebContents()->GetSiteInstance())
      ->SetZoomLevelForHostAndScheme(content::kChromeUIScheme,
                                     chrome::kChromeUICustomBrowserHost, 0);
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* html_source =
      content::WebUIDataSource::CreateAndAdd(
          profile, chrome::kChromeUICustomBrowserHost);
  webui::SetupWebUIDataSource(html_source, kCustomBrowserResources,
                              IDR_CUSTOM_BROWSER_PAGES_INDEX_HTML);

  html_source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FrameSrc, "frame-src 'self';");
  html_source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FrameAncestors,
      "frame-ancestors 'self';");

  CustomBrowserUiPrefs::InitMainUI(html_source, profile->GetPrefs());
  html_source->AddString("customBrowserProductVersion",
                         custom_browser::kCustomBrowserProductVersion);

  html_source->AddString("tabIdDataType", kCustomWebUITabIdDataType);
  html_source->AddString("tabGroupIdDataType", kCustomWebUITabGroupIdDataType);

  static constexpr webui::LocalizedString kStrings[] = {
      {"tabListTitle", IDS_ACCNAME_TAB_LIST},
      {"closeTab", IDS_ACCNAME_CLOSE},
      {"defaultTabTitle", IDS_DEFAULT_TAB_TITLE},
      {"loadingTab", IDS_TAB_LOADING_TITLE},
      {"tabCrashed", IDS_TAB_AX_LABEL_CRASHED_FORMAT},
      {"tabNetworkError", IDS_TAB_AX_LABEL_NETWORK_ERROR_FORMAT},
      {"audioPlaying", IDS_TAB_AX_LABEL_AUDIO_PLAYING_FORMAT},
      {"usbConnected", IDS_TAB_AX_LABEL_USB_CONNECTED_FORMAT},
      {"bluetoothConnected", IDS_TAB_AX_LABEL_BLUETOOTH_CONNECTED_FORMAT},
      {"hidConnected", IDS_TAB_AX_LABEL_HID_CONNECTED_FORMAT},
      {"serialConnected", IDS_TAB_AX_LABEL_SERIAL_CONNECTED_FORMAT},
      {"mediaRecording", IDS_TAB_AX_LABEL_MEDIA_RECORDING_FORMAT},
      {"audioRecording", IDS_TAB_AX_LABEL_AUDIO_RECORDING_FORMAT},
      {"videoRecording", IDS_TAB_AX_LABEL_VIDEO_RECORDING_FORMAT},
      {"audioMuting", IDS_TAB_AX_LABEL_AUDIO_MUTING_FORMAT},
      {"tabCapturing", IDS_TAB_AX_LABEL_DESKTOP_CAPTURING_FORMAT},
      {"pipPlaying", IDS_TAB_AX_LABEL_PIP_PLAYING_FORMAT},
      {"desktopCapturing", IDS_TAB_AX_LABEL_DESKTOP_CAPTURING_FORMAT},
      {"vrPresenting", IDS_TAB_AX_LABEL_VR_PRESENTING},
      {"unnamedGroupLabel", IDS_GROUP_AX_LABEL_UNNAMED_GROUP_FORMAT},
      {"namedGroupLabel", IDS_GROUP_AX_LABEL_NAMED_GROUP_FORMAT},
      {"newTabAccName", IDS_ACCNAME_NEWTAB},
  };
  html_source->AddLocalizedStrings(kStrings);
  AddNexusStrings(html_source);

  web_ui->AddMessageHandler(std::make_unique<settings::NexusSettingsHandler>());

  content::URLDataSource::Add(
      profile, std::make_unique<FaviconSource>(
                   profile, chrome::FaviconUrlFormat::kFavicon2));
}

CustomBrowserWebUI::~CustomBrowserWebUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(CustomBrowserWebUI)

void CustomBrowserWebUI::BindInterface(
    mojo::PendingReceiver<custom_browser::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void CustomBrowserWebUI::BindInterface(
    mojo::PendingReceiver<color_change_listener::mojom::PageHandler> receiver) {
  auto* render_frame_host = web_ui()->GetRenderFrameHost();
  if (!render_frame_host) {
    return;
  }
  auto* color_change_handler =
      ui::ColorChangeHandler::GetOrCreateForCurrentDocument(render_frame_host);
  color_change_handler->Bind(std::move(receiver));
}

// void CustomBrowserWebUIConfig::BindInterface(
//     mojo::PendingReceiver<tabs_api::mojom::TabStripService> receiver) {
//   if (auto* tab_strip_service =
//           browser_->browser_window_features()->tab_strip_service()) {
//     tab_strip_service->Accept(std::move(receiver));
//   }
// }

void CustomBrowserWebUI::CreatePageHandler(
    mojo::PendingRemote<custom_browser::mojom::Browser> browser,
    mojo::PendingRemote<custom_browser::mojom::UIView> native_view,
    mojo::PendingReceiver<custom_browser::mojom::BrowserHandler>
        browser_handler_receiver,
    mojo::PendingReceiver<custom_browser::mojom::UIViewHandler>
        native_view_handler_receiver) {
  // Initialize() must be called immediately after LoadURL() for the WebUI
  // Tab Strip to start correctly. Only create
  // CustomBrowserWebUIConfigPageHandler when both browser_ and embedder_ are
  // set after calling Initialize().
  if (custom_browser_view_) {
    custom_browser_handler_ = std::make_unique<CustomBrowserHandler>(
        std::move(browser_handler_receiver), std::move(browser), web_ui(),
        custom_browser_view_);

    native_view_handler_ = std::make_unique<UIViewHandler>(
        std::move(native_view_handler_receiver), std::move(native_view),
        custom_browser_view_);
  }
}

void CustomBrowserWebUI::Initialize(CustomBrowserView* custom_browser_view) {
  // content::WebUI* const web_ui = CustomBrowserWebUI::web_ui();
  // DCHECK_EQ(Profile::FromWebUI(web_ui),
  // custom_browser_view->browser()->profile());
  custom_browser_view_ = custom_browser_view;
}

void CustomBrowserWebUI::Deinitialize() {
  custom_browser_handler_.reset();
  native_view_handler_.reset();
  DCHECK(custom_browser_view_);
  custom_browser_view_ = nullptr;
}

void CustomBrowserWebUI::LayoutChanged() {
  if (custom_browser_handler_) {
    custom_browser_handler_->NotifyLayoutChanged();
  }
}

void CustomBrowserWebUI::ReceivedKeyboardFocus() {
  if (custom_browser_handler_) {
    custom_browser_handler_->NotifyReceivedKeyboardFocus();
  }
}
