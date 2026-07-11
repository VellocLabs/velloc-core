#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NEXUS_BOOTSTRAP_HOOKS_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NEXUS_BOOTSTRAP_HOOKS_H_

namespace mojo {
class GenericPendingReceiver;
}  // namespace mojo

namespace content {
class RenderFrameHost;
}  // namespace content

namespace custom_browser::patch_webui {

// Binds `nexus.mojom.NexusBridgeBootstrap` for the chrome://custom.top-chrome
// host page that owns the nexus_web <webview>. Hands the receiver to a
// `nexus::NexusBridgeBootstrapHost` bound to the frame.
bool BindNexusBridgeBootstrap(content::RenderFrameHost* render_frame_host,
                              mojo::GenericPendingReceiver* receiver);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_NEXUS_BOOTSTRAP_HOOKS_H_
