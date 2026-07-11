#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_CALLBACKS_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_CALLBACKS_H_

#include <optional>
#include <string_view>

#include "base/functional/callback.h"
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

// Notification-style hook triggered just before WebUI strings are baked into the
// IPC payload for the renderer. Ideal for replacing strings dynamically before the
// HTML is parsed by Blink.
using WebUIDataSourceConfiguringCallback =
    base::RepeatingCallback<void(content::WebUI*, content::WebUIDataSource*)>;

// Provider-style hook: registers throttles for a navigation.
// Repeating because it can fire multiple times.
using NavigationThrottleProviderCallback =
    base::RepeatingCallback<void(content::NavigationThrottleRegistry&)>;

// Populator-style hook for RenderFrame-scoped WebUI BinderMaps. Features use
// this to register all typed Mojo binders behind a single custom-browser entry
// point instead of exposing each interface from Chrome-owned files.
using WebUIRenderFrameBinderMapPopulatorCallback = base::RepeatingCallback<void(
    mojo::BinderMapWithContext<content::RenderFrameHost*>*)>;

// Override-style hook: optionally replaces the anchor rect for a WebUI bubble.
// Receives the anchor view and its default rect; returns a replacement or
// std::nullopt.
using BubbleAnchorRectOverrideCallback =
    base::RepeatingCallback<std::optional<gfx::Rect>(const views::View*,
                                                     const gfx::Rect&)>;

// Binder-style hook for RenderFrame interface requests. Returns true iff the
// callback claimed and bound the receiver. Implementations must not consume the
// receiver when returning false.
using RenderFrameInterfaceBinderCallback =
    base::RepeatingCallback<bool(content::RenderFrameHost*,
                                 mojo::GenericPendingReceiver*)>;

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_CALLBACKS_H_
