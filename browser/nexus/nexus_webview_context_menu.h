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

// Context-menu policy for the Nexus guest <webview> (`persist:nexus`), called
// from a guarded hook before the default webview menu. Always handles a
// Nexus guest: an editable target gets the native edit menu (Undo, Redo,
// Cut, Copy, Paste, Select All), `nexus.inspect_menu_enabled` ON adds
// "Inspect", and anything else shows no menu. The pref is read per
// right-click. Returns false for every non-Nexus guest (partition name + a
// chrome:// WebUI owner). (F-4)
bool MaybeHandleNexusGuestContextMenu(
    extensions::WebViewGuest* guest,
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBVIEW_CONTEXT_MENU_H_
