#ifndef CUSTOM_BROWSER_UI_VIEW_NEXUS_NATIVE_MENUS_H_
#define CUSTOM_BROWSER_UI_VIEW_NEXUS_NATIVE_MENUS_H_

class Browser;

namespace ui {
class SimpleMenuModel;
}  // namespace ui

namespace custom_browser {

// Builds the purpose-built system menu (right-click the top bar / taskbar
// thumbnail / Alt+Space) of the kNexus window and of a single-web window
// (F-29) into |model|, replacing the default tab-centric browser menu.
// Returns true iff |browser| is one of them (the caller then skips the
// default menu). Items use existing IDC commands so the existing
// SystemMenuModelDelegate routes them; only the labels are overridden.
// Returns false (no-op) for every other window, so normal browser windows
// keep the default menu.
bool BuildNexusSystemMenu(ui::SimpleMenuModel* model, Browser* browser);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_NEXUS_NATIVE_MENUS_H_
