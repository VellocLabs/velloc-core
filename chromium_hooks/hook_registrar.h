#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_REGISTRAR_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_REGISTRAR_H_

#include <memory>
#include <string_view>

#include "custom_browser/chromium_hooks/hook_callbacks.h"
#include "custom_browser/chromium_hooks/hook_types.h"

namespace custom_browser {

// Features register hooks through the registrar.
// The dispatcher owns the storage and fans out calls.
class HookRegistrar {
 public:
  virtual ~HookRegistrar() = default;

  virtual void AddWebUIDataSourceCreatedCallback(
      std::string_view host_path,
      WebUIDataSourceCreatedCallback cb) = 0;

  virtual void AddLocalizedStringOverrideCallback(
      LocalizedStringOverrideCallback cb) = 0;

  virtual void AddNavigationThrottleProvider(
      NavigationThrottleProviderCallback provider) = 0;

  virtual void AddBrowserMainLifecycleHook(
      std::unique_ptr<BrowserMainLifecycleHook> hook) = 0;
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_REGISTRAR_H_
