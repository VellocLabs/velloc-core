#include "custom_browser/features/patch_webui/register_hooks.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller_interface_binder.h"
#include "custom_browser/browser/nexus/mojom/nexus_bridge_bootstrap.mojom.h"
#include "custom_browser/chromium_hooks/hook_dispatcher.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/features/patch_webui/extensions_hooks.h"
#include "custom_browser/features/patch_webui/glic_hooks.h"
#include "custom_browser/features/patch_webui/navigation_hooks.h"
#include "custom_browser/features/patch_webui/ntp_hooks.h"
#include "custom_browser/features/patch_webui/settings_hooks.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "ui/base/resource/resource_bundle.h"

namespace custom_browser::patch_webui {

namespace {

#define CUSTOM_BROWSER_PATCH_WEBUI_RENDER_FRAME_INTERFACES(X) \
  X(nexus::mojom::NexusBridgeBootstrap)

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
      Interface::Name_, base::BindRepeating(&BindNexusBridgeBootstrapForGlic));
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
  LoadCustomBrowserStrings();
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
  registrar->AddWebUIDataSourceConfiguringCallback(
      chrome::kChromeUIGlicHost,
      base::BindRepeating(&OnGlicDataSourceConfiguring));
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
}

}  // namespace custom_browser::patch_webui
