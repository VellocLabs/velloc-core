#include "custom_browser/browser/nexus/nexus_guest_config.h"

#include <string>
#include <string_view>

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_webui_config.h"
#include "url/gurl.h"

namespace nexus {

namespace {

bool IsAllowedDevHost(std::string_view host) {
  return host == "localhost" || host == "127.0.0.1";
}

bool IsAllowedCustomGuestUrl(const GURL& url) {
  if (!url.is_valid() || !url.has_host()) {
    return false;
  }

  if (url.SchemeIs("https")) {
    return true;
  }

  if (!url.SchemeIs("http")) {
    return false;
  }

  return IsAllowedDevHost(url.host());
}

GURL GetDefaultGuestUrl() {
  return GURL(kDefaultNexusGuestURL);
}

// The `log` / `trace` URL-param mapping off the `nexus.agent_config`
// pref used to live here — layering `?log=1` / `?trace=1` onto the
// guest URL at panel load time. It's been removed: the nexus_web
// page-thread now reads those flags directly from
// `~/.nexus/settings.json` (under `agent.log` / `agent.trace`) via
// `loadAgentTracerFlags()` and merges them with any URL-param
// override via `combineTracerSignals()`. The URL params still work
// when explicitly set (dev / e2e), so the chrome harness's
// `?log=1` injection is unchanged — but the prod path is now
// "About toggle writes settings.json → next session boot's tracer
// install reads it" rather than "About toggle writes the C++ pref
// → C++ writes the URL → JS parses the URL."

}  // namespace

GURL GetResolvedNexusGuestURL(Profile* profile) {
  const GURL default_url = GetDefaultGuestUrl();

  if (!default_url.is_valid()) {
    LOG(ERROR) << "Default Nexus guest URL is invalid: "
               << kDefaultNexusGuestURL;
    return GURL();
  }

  if (!profile) {
    return default_url;
  }

  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return default_url;
  }

  // Resolve the base URL: the user's custom Agent URL override, else default.
  GURL base = default_url;
  const std::string custom_url_spec = prefs->GetString(prefs::kNexusWebappUrl);
  if (!custom_url_spec.empty()) {
    const GURL custom_url(custom_url_spec);
    if (IsAllowedCustomGuestUrl(custom_url)) {
      base = custom_url;
    } else {
      LOG(WARNING) << "Ignoring invalid Nexus guest URL: " << custom_url_spec;
    }
  }

  return base;
}

}  // namespace nexus
