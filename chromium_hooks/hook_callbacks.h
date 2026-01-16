#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_CALLBACKS_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_CALLBACKS_H_

#include <string_view>

#include "base/functional/callback.h"

namespace content {
class BrowserContext;
class WebUIDataSource;
class NavigationThrottleRegistry;
}  // namespace content

namespace custom_browser {

// Notification-style hook; suited for callbacks.
// Repeating because it can fire multiple times.
using WebUIDataSourceCreatedCallback =
    base::RepeatingCallback<void(content::BrowserContext*,
                                 content::WebUIDataSource*)>;

// Override-style hook; returns the resource id to use.
// Repeating because it can fire multiple times.
using LocalizedStringOverrideCallback =
    base::RepeatingCallback<int(content::WebUIDataSource*,
                                std::string_view,
                                int)>;

// Provider-style hook: registers throttles for a navigation.
// Repeating because it can fire multiple times.
using NavigationThrottleProviderCallback =
    base::RepeatingCallback<void(content::NavigationThrottleRegistry&)>;

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_CALLBACKS_H_
