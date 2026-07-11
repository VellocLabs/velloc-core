#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBVIEW_CONTEXT_MENU_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBVIEW_CONTEXT_MENU_H_

namespace content {
class RenderFrameHost;
struct ContextMenuParams;
}  // namespace content

namespace extensions {
class WebViewGuest;
}  // namespace extensions

namespace nexus {

// Context-menu policy for the Nexus panel guest <webview> — the
// `partition="persist:nexus"` guest that <nexus-view> (browser WebUI)
// hosts for the nexus_web app. Called from
// `ChromeWebViewGuestDelegate::HandleContextMenu` via a tiny
// ENABLE_CUSTOM_BROWSER-guarded hook, BEFORE the default webview menu
// pipeline (ContextMenuDelegate::BuildMenuAsync → embedder
// `contextMenuShow` event → default RenderViewContextMenu).
//
// Returns true when the request was handled and the default pipeline must
// not run:
//   - `nexus.inspect_menu_enabled` pref OFF (the default): the event is
//     swallowed — the panel shows NO context menu at all (it is a product
//     surface, not a page).
//   - pref ON: shows a single-item "Inspect" menu; activating it opens
//     DevTools on the guest at the clicked element
//     (`DevToolsWindow::InspectElement`).
// The pref is read per right-click, so `/debug inspect on|off` applies
// immediately — no reload.
//
// Returns false for every non-Nexus guest (identified by storage-partition
// name + a chrome:// WebUI owner) so regular <webview> behavior is
// untouched.
bool MaybeHandleNexusGuestContextMenu(
    extensions::WebViewGuest* guest,
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBVIEW_CONTEXT_MENU_H_
