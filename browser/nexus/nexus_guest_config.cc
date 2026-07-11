#include "custom_browser/browser/nexus/nexus_guest_config.h"

#include <string>
#include <string_view>

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_webui_config.h"
#include "content/public/common/url_constants.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr std::string_view kNexusModeQueryKey = "openclaw";
constexpr std::string_view kNexusModeQueryValue = "1";

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

bool IsNexusGlicMode(const GURL& webui_url) {
  if (!webui_url.is_valid() || !webui_url.SchemeIs(content::kChromeUIScheme) ||
      webui_url.host() != chrome::kChromeUIGlicHost) {
    return false;
  }

  std::string query_value;
  return net::GetValueForKeyInQuery(webui_url, kNexusModeQueryKey,
                                    &query_value) &&
         query_value == kNexusModeQueryValue;
}

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
