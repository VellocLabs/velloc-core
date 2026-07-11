#include "custom_browser/features/hooks_registration.h"

#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/features/patch_webui/register_hooks.h"

namespace custom_browser::features {

void RegisterAllHooks(custom_browser::HookRegistrar* registrar) {
  // Register the Nexus keyed-service factory before Chrome's profile init
  // builds the BrowserContextKeyedServiceFactory graph.
  nexus::LlmBrokerFactory::GetInstance();
  patch_webui::RegisterHooks(registrar);
}

}  // namespace custom_browser::features
