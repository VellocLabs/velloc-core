#include "custom_browser/browser/nexus/nexus_webui_config.h"

#include "base/logging.h"
#include "build/buildflag.h"

namespace nexus {

#if defined(NDEBUG)
const char kDefaultNexusGuestURL[] = "https://nexus.velloc.app/index.html";
#else
const char kDefaultNexusGuestURL[] =
    "http://localhost:5173/index.html?log=1&trace=1&logCallModel=1";
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
