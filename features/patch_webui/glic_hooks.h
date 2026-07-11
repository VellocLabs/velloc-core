#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_GLIC_HOOKS_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_GLIC_HOOKS_H_

namespace mojo {
class GenericPendingReceiver;
}  // namespace mojo

namespace content {
class BrowserContext;
class RenderFrameHost;
class WebUI;
class WebUIDataSource;
}  // namespace content

namespace custom_browser::patch_webui {

void OnGlicDataSourceConfiguring(content::WebUI* web_ui,
                                 content::WebUIDataSource* data_source);

bool BindNexusBridgeBootstrapForGlic(
    content::RenderFrameHost* render_frame_host,
    mojo::GenericPendingReceiver* receiver);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_GLIC_HOOKS_H_
