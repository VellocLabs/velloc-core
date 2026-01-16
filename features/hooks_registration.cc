#include "custom_browser/features/hooks_registration.h"

#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/features/patch_webui/register_hooks.h"

namespace custom_browser::features {

void RegisterAllHooks(custom_browser::HookRegistrar* registrar) {
  patch_webui::RegisterHooks(registrar);
}

}  // namespace custom_browser::features
