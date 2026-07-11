#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TOOLBAR_BUTTON_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TOOLBAR_BUTTON_H_

#include <string>

namespace nexus {

// Manages the Nexus toolbar button that sits next to the GLIC button in the
// browser toolbar. Clicking the button toggles the chrome://nexus panel
// independently of the chrome://glic panel.
//
// The button uses the Nexus icon from resources/images/nexus_panel_icon.png
// and is gated behind BUILDFLAG(ENABLE_CUSTOM_BROWSER).
//
// This class provides the model / metadata for the toolbar button. The
// actual toolbar integration (creating the Views button and inserting it
// next to GLIC) is handled by the upstream patch
// `nexus-toolbar-button.patch` (T032) which calls CreateNexusToolbarButton().
//
// FR-000c: Each panel MUST have its own distinct toolbar button.

// String constants for the button.
extern const char kNexusToolbarButtonId[];
extern const char kNexusToolbarButtonTooltip[];

// Called from the upstream toolbar patch to create the Nexus button.
// Returns true if the button was successfully configured.
bool CreateNexusToolbarButton();

// Returns true if the Nexus toolbar button has been created.
bool IsNexusToolbarButtonCreated();

// Returns the resource ID for the Nexus toolbar button icon.
int GetNexusToolbarButtonIconResourceId();

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TOOLBAR_BUTTON_H_
