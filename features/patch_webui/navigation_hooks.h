#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NAVIGATION_HOOKS_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NAVIGATION_HOOKS_H_

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

namespace custom_browser::patch_webui {

void AddSearchRedirectThrottle(
    content::NavigationThrottleRegistry& registry);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NAVIGATION_HOOKS_H_
