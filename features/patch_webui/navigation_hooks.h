#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NAVIGATION_HOOKS_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NAVIGATION_HOOKS_H_

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

namespace custom_browser::patch_webui {

void AddSearchRedirectThrottle(
    content::NavigationThrottleRegistry& registry);

// Captures the loopback OAuth redirect (http://localhost:1455/auth/callback)
// for subscription-backed LLM providers (e.g. ChatGPT Plus/Pro): extracts the
// authorization code, hands it to the profile's NexusOAuthManager, and
// replaces the (dead) loopback navigation with a friendly result page. The
// browser captures the redirect this way instead of binding a local socket.
void AddNexusOAuthRedirectThrottle(
    content::NavigationThrottleRegistry& registry);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NAVIGATION_HOOKS_H_
