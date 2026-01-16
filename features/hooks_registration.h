#ifndef CUSTOM_BROWSER_FEATURES_HOOKS_REGISTRATION_H_
#define CUSTOM_BROWSER_FEATURES_HOOKS_REGISTRATION_H_

namespace custom_browser {
class HookRegistrar;
}

namespace custom_browser::features {

// Called once by the dispatcher during startup.
void RegisterAllHooks(custom_browser::HookRegistrar* registrar);

}  // namespace custom_browser::features

#endif  // CUSTOM_BROWSER_FEATURES_HOOKS_REGISTRATION_H_
