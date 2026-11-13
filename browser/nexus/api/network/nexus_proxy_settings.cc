// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_proxy_settings.h"

#include <string_view>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_prefs.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"
#include "net/base/proxy_string_util.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr char kModeSystem[] = "system";
constexpr char kModeDirect[] = "direct";
constexpr char kModeFixed[] = "fixed";
constexpr char kModePac[] = "pac";
constexpr char kModeAutoDetect[] = "autoDetect";

base::unexpected<ProxySettingsProblem> Refuse(const char* field,
                                              const char* reason) {
  return base::unexpected(ProxySettingsProblem{field, reason});
}

bool HasControlCharacter(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
}

const char* WireMode(ProxyPrefs::ProxyMode mode) {
  switch (mode) {
    case ProxyPrefs::MODE_DIRECT:
      return kModeDirect;
    case ProxyPrefs::MODE_AUTO_DETECT:
      return kModeAutoDetect;
    case ProxyPrefs::MODE_PAC_SCRIPT:
      return kModePac;
    case ProxyPrefs::MODE_FIXED_SERVERS:
      return kModeFixed;
    default:
      return kModeSystem;
  }
}

const char* WireSource(const PrefService::Preference& pref) {
  if (pref.IsManaged() || pref.IsManagedByCustodian()) {
    return "policy";
  }
  if (pref.IsExtensionControlled()) {
    return "extension";
  }
  if (!pref.IsUserModifiable()) {
    return "commandLine";
  }
  return pref.IsUserControlled() ? "user" : "default";
}

}  // namespace

ProxySettingsRequest::ProxySettingsRequest() = default;
ProxySettingsRequest::ProxySettingsRequest(ProxySettingsRequest&&) = default;
ProxySettingsRequest& ProxySettingsRequest::operator=(ProxySettingsRequest&&) =
    default;
ProxySettingsRequest::~ProxySettingsRequest() = default;

base::expected<std::optional<base::DictValue>, ProxySettingsProblem>
BuildProxyPreference(const ProxySettingsRequest& request) {
  if (request.mode == kModeSystem) {
    return std::optional<base::DictValue>();
  }
  if (request.mode == kModeDirect) {
    return std::optional<base::DictValue>(
        ProxyConfigDictionary::CreateDirect());
  }
  if (request.mode == kModeFixed) {
    const auto endpoint = ParseProxyEndpoint(request.server.value_or(""),
                                             ProxyCredentials::kRefuse);
    if (!endpoint.has_value()) {
      return Refuse("server", ProxyEndpointProblemWireName(endpoint.error()));
    }
    const std::string bypass(base::TrimWhitespaceASCII(
        request.bypass_list.value_or(""), base::TRIM_ALL));
    if (bypass.size() > kMaxProxyBypassListChars) {
      return Refuse("bypassList", "tooLong");
    }
    if (HasControlCharacter(bypass)) {
      return Refuse("bypassList", "malformed");
    }
    return std::optional<base::DictValue>(
        ProxyConfigDictionary::CreateFixedServers(
            net::ProxyServerToProxyUri(endpoint->server), bypass));
  }
  if (request.mode == kModePac) {
    const GURL url(base::TrimWhitespaceASCII(request.pac_url.value_or(""),
                                             base::TRIM_ALL));
    if (!url.is_valid() || url.has_username() || url.has_password()) {
      return Refuse("pacUrl", "malformed");
    }
    if (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIsFile()) {
      return Refuse("pacUrl", "scheme");
    }
    return std::optional<base::DictValue>(
        ProxyConfigDictionary::CreatePacScript(url.spec(),
                                               /*pac_mandatory=*/false));
  }
  return Refuse("mode", "malformed");
}

base::DictValue DescribeProxyPreference(const PrefService& prefs) {
  base::DictValue record;
  const PrefService::Preference* pref =
      prefs.FindPreference(proxy_config::prefs::kProxy);
  if (!pref || !pref->GetValue()->is_dict()) {
    record.Set("mode", kModeSystem);
    record.Set("source", "default");
    record.Set("editable", false);
    return record;
  }
  const ProxyConfigDictionary stored(pref->GetValue()->GetDict().Clone());
  ProxyPrefs::ProxyMode mode = ProxyPrefs::MODE_SYSTEM;
  stored.GetMode(&mode);
  record.Set("mode", WireMode(mode));
  std::string text;
  if (mode == ProxyPrefs::MODE_FIXED_SERVERS) {
    if (stored.GetProxyServer(&text)) {
      // One server for every scheme is stored bare when it is an HTTP proxy.
      const bool bare = text.find("://") == std::string::npos &&
                        text.find('=') == std::string::npos &&
                        text.find(';') == std::string::npos;
      record.Set("server", bare ? base::StrCat({"http://", text}) : text);
    }
    if (stored.GetBypassList(&text)) {
      record.Set("bypassList", text);
    }
  } else if (mode == ProxyPrefs::MODE_PAC_SCRIPT && stored.GetPacUrl(&text)) {
    record.Set("pacUrl", text);
  }
  record.Set("source", WireSource(*pref));
  record.Set("editable", pref->IsUserModifiable());
  return record;
}

bool IsProxyPreferenceEditable(const PrefService& prefs) {
  const PrefService::Preference* pref =
      prefs.FindPreference(proxy_config::prefs::kProxy);
  return pref && pref->IsUserModifiable();
}

}  // namespace nexus
