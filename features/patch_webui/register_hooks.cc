#include "custom_browser/features/patch_webui/register_hooks.h"

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller_interface_binder.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_url_opener.h"
#include "custom_browser/browser/nexus/mojom/nexus_bridge_bootstrap.mojom.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "custom_browser/chromium_hooks/hook_dispatcher.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/features/patch_webui/custom_browser_strings_hook.h"
#include "custom_browser/features/patch_webui/extensions_hooks.h"
#include "custom_browser/features/patch_webui/nexus_bootstrap_hooks.h"
#include "custom_browser/features/patch_webui/navigation_hooks.h"
#include "custom_browser/features/patch_webui/ntp_hooks.h"
#include "custom_browser/features/patch_webui/settings_hooks.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "ui/base/resource/resource_bundle.h"

namespace custom_browser::patch_webui {

namespace {

#define CUSTOM_BROWSER_PATCH_WEBUI_RENDER_FRAME_INTERFACES(X) \
  X(nexus::mojom::NexusBridgeBootstrap)

std::string GetApplicationLocale() {
  return g_browser_process ? g_browser_process->GetApplicationLocale()
                           : std::string();
}

void AddCustomBrowserStringsPak(const base::FilePath& pak_path) {
  ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
      pak_path, ui::kScaleFactorNone);
}

void RegisterCustomBrowserStrings(HookRegistrar* registrar) {
  base::FilePath assets_path;
  if (!base::PathService::Get(base::DIR_ASSETS, &assets_path)) {
    return;
  }
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<CustomBrowserStringsHook>(
          std::move(assets_path), base::BindRepeating(&GetApplicationLocale),
          base::BindRepeating(&AddCustomBrowserStringsPak)));
}

template <typename Interface>
void BindHookedRenderFrameInterface(
    content::RenderFrameHost* host,
    mojo::PendingReceiver<Interface> receiver) {
  if (custom_browser::TryBindRenderFrameInterface(
          host, mojo::GenericPendingReceiver(std::move(receiver)))) {
    return;
  }

  content::internal::ReceivedInvalidWebUIControllerMessage(host);
}

template <typename Interface>
void RegisterPatchWebUIRenderFrameInterface(HookRegistrar* registrar) {
  registrar->AddRenderFrameInterfaceBinder(
      Interface::Name_, base::BindRepeating(&BindNexusBridgeBootstrap));
}

template <typename Interface>
void AddPatchWebUIRenderFrameBinder(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  map->Add<Interface>(&BindHookedRenderFrameInterface<Interface>);
}

void PopulatePatchWebUIRenderFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
#define ADD_PATCH_WEBUI_BINDER(Interface) AddPatchWebUIRenderFrameBinder<Interface>(map);
  CUSTOM_BROWSER_PATCH_WEBUI_RENDER_FRAME_INTERFACES(ADD_PATCH_WEBUI_BINDER)
#undef ADD_PATCH_WEBUI_BINDER
}

}  // namespace

void RegisterHooks(custom_browser::HookRegistrar* registrar) {
  RegisterCustomBrowserStrings(registrar);
  registrar->AddWebUIDataSourceConfiguringCallback(
      chrome::kChromeUINewTabPageHost,
      base::BindRepeating(&OnNewTabPageDataSourceConfiguring));
  registrar->AddWebUIDataSourceConfiguringCallback(
      chrome::kChromeUISettingsHost,
      base::BindRepeating(&OnSettingsDataSourceConfiguring));
  registrar->AddWebUIDataSourceConfiguringCallback(
      chrome::kChromeUIExtensionsHost,
      base::BindRepeating(&OnExtensionsDataSourceConfiguring));
  registrar->AddWebUIDataSourceConfiguringCallback(
      chrome::kChromeUICustomizeChromeSidePanelHost,
      base::BindRepeating(&OnCustomizeChromeSidePanelDataSourceConfiguring));
  registrar->AddWebUIRenderFrameBinderMapPopulator(
      base::BindRepeating(&PopulatePatchWebUIRenderFrameBinders));
#define REGISTER_PATCH_WEBUI_INTERFACE(Interface) \
  RegisterPatchWebUIRenderFrameInterface<Interface>(registrar);
  CUSTOM_BROWSER_PATCH_WEBUI_RENDER_FRAME_INTERFACES(
      REGISTER_PATCH_WEBUI_INTERFACE)
#undef REGISTER_PATCH_WEBUI_INTERFACE
  registrar->AddWebUIDataSourceConfiguringCallback(
      chrome::kChromeUIVersionHost,
      base::BindRepeating(&OnVersionDataSourceConfiguring));
  registrar->AddNavigationThrottleProvider(
      base::BindRepeating(&AddSearchRedirectThrottle));
  registrar->AddNavigationThrottleProvider(
      base::BindRepeating(&AddNexusOAuthRedirectThrottle));

  // Wire the OAuth "Sign in" tab opener. The settings WebUI handler cannot
  // depend on //chrome/browser/ui (it would form a GN cycle), so it opens the
  // authorization URL through this hook, backed by window_util here.
  nexus::SetNexusAuthUrlOpener(
      base::BindRepeating(&custom_browser::window_util::OpenUrlInNormalWindow));
}

}  // namespace custom_browser::patch_webui
