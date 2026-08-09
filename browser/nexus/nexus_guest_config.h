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

// Appends the compiled-in bridge interface version segment ("v<N>/",
// nexus_interface_version.h) to an https base URL:
//   https://app.velloc.app/        -> https://app.velloc.app/v1/
//   https://mirror.example/nexus   -> https://mirror.example/nexus/v1/
// Pass-through cases:
//   - http bases (localhost dev server: unversioned path, the guest skips
//     its URL version check; drift is caught by the ninja build gate),
//   - bases already ending in a /v<digits>/ segment (a hand-pasted
//     version-pinned URL; the guest still validates the pinned number).
// Exposed for the custom_browser_nexus_browser_unittests regression tests.
GURL AppendNexusInterfaceVersionToBase(const GURL& base);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_CONFIG_H_
