#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_REGISTER_HOOKS_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_REGISTER_HOOKS_H_

namespace custom_browser {
class HookRegistrar;
}

namespace custom_browser::patch_webui {

void RegisterHooks(custom_browser::HookRegistrar* registrar);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_REGISTER_HOOKS_H_
