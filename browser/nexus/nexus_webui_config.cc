#include "custom_browser/browser/nexus/nexus_webui_config.h"

#include "base/logging.h"
#include "build/buildflag.h"

namespace nexus {

// Base guest URL only — diagnostic/feature query params (log, trace,
// toolDeferral) are layered on at resolve time by GetResolvedNexusGuestURL()
// based on the user's About-page toggles, rather than baked into the URL here.
//
// The release literal goes through a macro so it can ALSO be exported
// unconditionally as kReleaseNexusGuestURL. Without that, the one string whose
// corruption breaks every shipped binary is invisible to the build config
// developers actually use: a Debug binary does not contain it at all, so a
// test asserting on kDefaultNexusGuestURL there only ever sees the localhost
// value. The macro keeps the two definitions from drifting apart.
#define NEXUS_RELEASE_GUEST_URL "https://app.velloc.app/"

// Always defined, in every build config — see above. Not a fallback and not
// used at runtime; GetResolvedNexusGuestURL() reads kDefaultNexusGuestURL.
const char kReleaseNexusGuestURL[] = NEXUS_RELEASE_GUEST_URL;

#if defined(NDEBUG)
const char kDefaultNexusGuestURL[] = NEXUS_RELEASE_GUEST_URL;
#else
const char kDefaultNexusGuestURL[] = "http://localhost:5173/index.html";
#endif
const char kNexusHost[] = "nexus";

namespace {
bool g_nexus_webui_registered = false;
}  // namespace

void RegisterNexusWebUI() {
  if (g_nexus_webui_registered) {
    DLOG(WARNING) << "Nexus WebUI already registered";
    return;
  }

  // The actual WebUI controller factory registration is wired in via the
  // upstream patch `nexus-webui-registration.patch` (T032), which inserts a
  // call to this function during WebUI controller factory initialisation.
  //
  // This function sets up the chrome://nexus host that maps to the same
  // chrome://glic resource bundle. The mapping allows Nexus to share the
  // host page, tools layer, and WebUI controller with GLIC, while the
  // guest URL resolution branches on the active host (see guest_util.cc
  // override in T029).
  //
  // For now, this marks the registration as complete. The actual factory
  // wiring requires the upstream patch, which is implemented in Phase 4
  // (T032). The infrastructure is established here so that downstream code
  // can check IsNexusWebUIRegistered().

  g_nexus_webui_registered = true;
  DVLOG(1) << "Nexus WebUI host registered: chrome://" << kNexusHost;
}

bool IsNexusWebUIRegistered() {
  return g_nexus_webui_registered;
}

}  // namespace nexus
