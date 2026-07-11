#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_

#include "url/gurl.h"

class Profile;

namespace nexus {

// Returns true when the chrome://glic WebUI is running in Nexus mode.
bool IsNexusGlicMode(const GURL& webui_url);

// Returns the validated Nexus guest URL for the given profile. Invalid custom
// URLs fall back to kDefaultNexusGuestURL.
GURL GetResolvedNexusGuestURL(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_
