#include "custom_browser/features/hooks_registration.h"

#include <memory>

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_registration.h"
#include "custom_browser/browser/nexus/launcher/nexus_background_mode_manager.h"
#include "custom_browser/browser/nexus/nexus_installer_channel_registration.h"
#include "custom_browser/browser/nexus/panel/nexus_guest_priority_voter.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/features/nexus_keyed_service_registration.h"
#include "custom_browser/features/patch_webui/register_hooks.h"

namespace custom_browser::features {

void RegisterAllHooks(custom_browser::HookRegistrar* registrar) {
  patch_webui::RegisterHooks(registrar);

  // Registers the Nexus keyed-service factories with the
  // BrowserContextDependencyManager at PreCreateThreads. It cannot happen
  // here: RegisterAllHooks() runs from the CustomBrowserMainExtraParts
  // constructor, i.e. inside ChromeBrowserMainParts::Create(), which is before
  // BrowserProcess exists — and NexusBackgroundPanelHostFactory's
  // DependsOn(ThemeServiceFactory) reaches ChromeExtensionSystemFactory, whose
  // ctor DCHECKs on ExtensionsBrowserClient::Get(). Both ends of the window
  // are spelled out in nexus_keyed_service_registration.h.
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<nexus::NexusKeyedServiceLifecycleHook>());

  // Nexus launcher background mode: owns a NexusBackgroundModeManager for the
  // browser-process lifetime so toggling prefs::kBackgroundModeEnabled
  // holds/releases a keep-alive (system-tray icon + global hotkey land in
  // later stages).
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<nexus::NexusLauncherLifecycleHook>());

  // Keeps the Nexus guest's renderers (UI, runtime SharedWorker, sandboxed
  // mod-worker frames) at foreground priority while hidden or parked (P-2).
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<nexus::NexusGuestPriorityLifecycleHook>());

  // Answers guest HTTP-cache misses from the installer's bundle (F-33).
  nexus::RegisterBootBundleHooks(registrar);

  // Picks up the runtime channel a beta installer recorded (G-3).
  nexus::RegisterInstallerChannelHooks(registrar);
}

}  // namespace custom_browser::features
