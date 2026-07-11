#include "custom_browser/features/hooks_registration.h"

#include <memory>

#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/launcher/nexus_background_mode_manager.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tracker.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/features/patch_webui/register_hooks.h"

namespace custom_browser::features {

void RegisterAllHooks(custom_browser::HookRegistrar* registrar) {
  // Register the Nexus keyed-service factories before Chrome's profile init
  // builds the BrowserContextKeyedServiceFactory graph. A factory whose
  // GetInstance() is first reached lazily (e.g. NexusSessionTabTracker from
  // the bridge adapter at GuestReady) DCHECK-fails in dependency_manager.cc
  // because that's after EnsureBrowserContextKeyedServiceFactoriesBuilt().
  nexus::LlmBrokerFactory::GetInstance();
  nexus::NexusSessionTabTrackerFactory::GetInstance();
  patch_webui::RegisterHooks(registrar);

  // Nexus launcher background mode: owns a NexusBackgroundModeManager for the
  // browser-process lifetime so toggling nexus.launcher_enabled holds/releases
  // a keep-alive (system-tray icon + global hotkey land in later stages).
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<nexus::NexusLauncherLifecycleHook>());
}

}  // namespace custom_browser::features
