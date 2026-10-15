#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_

#include "url/gurl.h"

class Profile;

namespace nexus {

// Returns the validated Nexus guest URL for the given profile. Invalid custom
// URLs fall back to kDefaultNexusGuestURL. The configured URL (pref or
// default) is a BASE: https bases get the bridge interface version path
// appended via AppendNexusInterfaceVersionToBase() so the binary only ever
// requests the web build matching its own contract.
GURL GetResolvedNexusGuestURL(Profile* profile);

// Appends the compiled-in interface version segment "v<N>/" to an https base
// URL (https://app.velloc.app/ -> https://app.velloc.app/v1/). http bases and
// bases already ending in a /v<digits>/ segment pass through unchanged.
GURL AppendNexusInterfaceVersionToBase(const GURL& base);

// True until the guest runtime has once finished booting on `profile`
// (MarkNexusRuntimeBooted). A null profile, or one without prefs, is not a
// first run.
bool IsNexusRuntimeFirstRun(Profile* profile);

// Records that the guest runtime finished booting on `profile`. Idempotent;
// a null profile is a no-op.
void MarkNexusRuntimeBooted(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_
