#include "custom_browser/browser/nexus/nexus_toolbar_button.h"

#include "base/logging.h"
#include "build/buildflag.h"

namespace nexus {

const char kNexusToolbarButtonId[] = "nexus-panel-button";
const char kNexusToolbarButtonTooltip[] = "Nexus AI Agent";

namespace {
bool g_toolbar_button_created = false;
}  // namespace

bool CreateNexusToolbarButton() {
  if (g_toolbar_button_created) {
    DLOG(WARNING) << "Nexus toolbar button already created";
    return true;
  }

  // The Nexus toolbar button is placed next to the GLIC button in the
  // browser toolbar. It toggles the chrome://nexus panel independently.
  //
  // The actual Views-level button creation and insertion into the toolbar
  // layout is handled by the upstream patch `nexus-toolbar-button.patch`
  // (T032), which calls this function to initialise the button model.
  //
  // The button uses:
  //   - Icon: resources/images/nexus_panel_icon.png (T005)
  //   - Tooltip: kNexusToolbarButtonTooltip
  //   - Action: Toggle chrome://nexus panel (via GlicWindowController)
  //
  // For the foundational phase, this function marks the button as created
  // and returns success. The Views integration requires the upstream patch.

  g_toolbar_button_created = true;
  DVLOG(1) << "Nexus toolbar button created: " << kNexusToolbarButtonId;
  return true;
}

bool IsNexusToolbarButtonCreated() {
  return g_toolbar_button_created;
}

int GetNexusToolbarButtonIconResourceId() {
  // Returns a placeholder resource ID. The actual resource ID will be
  // defined when the nexus_panel_icon.png resource is wired into the
  // resource pak via GN grit()/repack() targets.
  // TODO(nexus): Replace with actual IDR_NEXUS_PANEL_ICON once resources
  // are integrated.
  return 0;
}

}  // namespace nexus
