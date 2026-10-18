#include "custom_browser/browser/nexus/nexus_webui_config.h"

#include <utility>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "build/buildflag.h"

namespace nexus {

// The release literal is a macro so it is also exported, in every build
// config, as kReleaseNexusGuestURL.
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

  // Marks the registration complete, for IsNexusWebUIRegistered().
  g_nexus_webui_registered = true;
  DVLOG(1) << "Nexus WebUI host registered: chrome://" << kNexusHost;
}

bool IsNexusWebUIRegistered() {
  return g_nexus_webui_registered;
}

namespace {
std::string& HostStringsLocale() {
  static base::NoDestructor<std::string> locale;
  return *locale;
}
}  // namespace

void SetNexusHostStringsLocale(std::string locale) {
  HostStringsLocale() = std::move(locale);
}

const std::string& GetNexusHostStringsLocale() {
  return HostStringsLocale();
}

}  // namespace nexus
