#include "custom_browser/browser/nexus/nexus_guest_config.h"

#include <string>
#include <string_view>

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/llm/llm_pref_names.h"
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

  const std::string custom_url_spec = prefs->GetString(prefs::kNexusWebappUrl);
  if (custom_url_spec.empty()) {
    return default_url;
  }

  const GURL custom_url(custom_url_spec);
  if (IsAllowedCustomGuestUrl(custom_url)) {
    return custom_url;
  }

  LOG(WARNING) << "Ignoring invalid Nexus guest URL: " << custom_url_spec;
  return default_url;
}

}  // namespace nexus
