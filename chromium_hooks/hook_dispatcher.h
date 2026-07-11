#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_DISPATCHER_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_DISPATCHER_H_

#include <optional>
#include <string_view>

#include "custom_browser/chromium_hooks/chromium_hooks_export.h"
#include "ui/gfx/geometry/rect.h"

namespace mojo {
template <typename ContextType>
class BinderMapWithContext;
class GenericPendingReceiver;
}  // namespace mojo

namespace views {
class View;
}  // namespace views

namespace content {
class BrowserContext;
class NavigationThrottleRegistry;
class RenderFrameHost;
class WebUI;
class WebUIDataSource;
}  // namespace content

namespace custom_browser {

class HookRegistrar;

CUSTOM_BROWSER_HOOKS_EXPORT HookRegistrar* GetHookRegistrar();

// ---------- WebUI data source configuring hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT void OnWebUIConfiguringLocalResource(
    content::WebUI* web_ui,
    content::WebUIDataSource* data_source);

// ---------- Navigation throttles hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT void AppendNavigationThrottles(
    content::NavigationThrottleRegistry& registry);

// ---------- WebUI RenderFrame BinderMap population hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT void PopulateWebUIRenderFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

// ---------- RenderFrame interface binding hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT bool TryBindRenderFrameInterface(
    content::RenderFrameHost* render_frame_host,
    mojo::GenericPendingReceiver receiver);

// ---------- Browser main lifecycle hooks ----------
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPreCreateThreadsHooks();
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPostCreateThreadsHooks();
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPreMainMessageLoopRunHooks();
CUSTOM_BROWSER_HOOKS_EXPORT void RunBrowserMainPostMainMessageLoopRunHooks();

// ---------- Bubble anchor rect override hook ----------
CUSTOM_BROWSER_HOOKS_EXPORT std::optional<gfx::Rect> OnGetBubbleAnchorRect(
    const views::View* anchor_view,
    const gfx::Rect& default_rect);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_DISPATCHER_H_
