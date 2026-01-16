#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_DISPATCHER_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_DISPATCHER_H_

#include <string_view>

#include "custom_browser/chromium_hooks/chromium_hooks_export.h"

namespace content {
class BrowserContext;
class WebUIDataSource;
class NavigationThrottleRegistry;
}  // namespace content

namespace custom_browser {

class HookRegistrar;

CUSTOM_BROWSER_HOOKS_EXPORT HookRegistrar* GetHookRegistrar();

// ---------- WebUI data source hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT void OnWebUIDataSourceCreated(
    content::BrowserContext* browser_context,
    std::string_view host,
    content::WebUIDataSource* data_source);

// Deprecated alias for OnWebUIDataSourceCreated.
CUSTOM_BROWSER_HOOKS_EXPORT void NotifyWebUIDataSourceCreated(
    content::BrowserContext* browser_context,
    std::string_view host,
    content::WebUIDataSource* data_source);

// ---------- Localized string override hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT int OnAddLocalizedString(
    content::WebUIDataSource* data_source,
    std::string_view name,
    int ids);

// ---------- Navigation throttles hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT void AppendNavigationThrottles(
    content::NavigationThrottleRegistry& registry);

// ---------- Browser main lifecycle hooks ----------
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPreCreateThreadsHooks();
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPostCreateThreadsHooks();
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPreMainMessageLoopRunHooks();
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPostMainMessageLoopRunHooks();


}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_DISPATCHER_H_
