#include "custom_browser/browser/nexus/nexus_guest_config.h"

#include <string>
#include <string_view>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_interface_version.h"
#include "custom_browser/browser/nexus/nexus_webui_config.h"
#include "url/gurl.h"
#include "url/url_constants.h"

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

// True when `path` already ends in a "/v<digits>/" segment — i.e. the base
// is a hand-pasted version-pinned URL that must not get a second segment
// appended. Mirrors the guest's TRAILING_VERSION_SEGMENT parse
// (nexus_web/src/browser/bridge/protocol/interfaceVersion.ts).
bool EndsWithInterfaceVersionSegment(std::string_view path) {
  if (path.size() < 4 || path.back() != '/') {  // shortest match: "/v1/"
    return false;
  }
  path.remove_suffix(1);
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    return false;
  }
  std::string_view segment = path.substr(slash + 1);
  if (segment.size() < 2 || segment[0] != 'v') {
    return false;
  }
  segment.remove_prefix(1);
  for (const char c : segment) {
    if (!base::IsAsciiDigit(c)) {
      return false;
    }
  }
  return true;
}

}  // namespace

GURL AppendNexusInterfaceVersionToBase(const GURL& base) {
  // http is only reachable for localhost dev bases (IsAllowedCustomGuestUrl
  // and the debug default) — the Vite dev server serves at `/` with no
  // version segment, so dev bases pass through verbatim.
  if (!base.is_valid() || !base.SchemeIs(url::kHttpsScheme)) {
    return base;
  }

  std::string path(base.path());
  if (EndsWithInterfaceVersionSegment(path)) {
    return base;
  }
  if (path.empty() || path.back() != '/') {
    path += '/';
  }
  path += "v" NEXUS_IF_VERSION_STR(NEXUS_INTERFACE_VERSION) "/";

  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  return base.ReplaceComponents(replacements);
}

GURL GetResolvedNexusGuestURL(Profile* profile) {
  const GURL default_url = GetDefaultGuestUrl();

  if (!default_url.is_valid()) {
    LOG(ERROR) << "Default Nexus guest URL is invalid: "
               << kDefaultNexusGuestURL;
    return GURL();
  }

  // Resolve the base URL: the user's custom Agent URL override, else default.
  GURL base = default_url;
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (prefs) {
    const std::string custom_url_spec =
        prefs->GetString(prefs::kNexusWebappUrl);
    if (!custom_url_spec.empty()) {
      const GURL custom_url(custom_url_spec);
      if (IsAllowedCustomGuestUrl(custom_url)) {
        base = custom_url;
      } else {
        LOG(WARNING) << "Ignoring invalid Nexus guest URL: " << custom_url_spec;
      }
    }
  }

  // The configured URL is a BASE; the binary always pins the guest to its
  // own bridge interface version by path so an updated web deploy can never
  // silently change the contract under a shipped binary.
  return AppendNexusInterfaceVersionToBase(base);
}

bool IsNexusRuntimeFirstRun(Profile* profile) {
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  return prefs && !prefs->GetBoolean(prefs::kNexusRuntimeFirstBootCompleted);
}

void MarkNexusRuntimeBooted(Profile* profile) {
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (prefs) {
    prefs->SetBoolean(prefs::kNexusRuntimeFirstBootCompleted, true);
  }
}

}  // namespace nexus
