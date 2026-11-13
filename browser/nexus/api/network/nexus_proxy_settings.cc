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
#include "net/base/host_port_pair.h"
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

// The one http or https proxy a stored fixed-servers text names.
std::optional<net::HostPortPair> SignInProxyOf(const std::string& text) {
  const bool bare = text.find("://") == std::string::npos;
  const auto endpoint = ParseProxyEndpoint(
      bare ? base::StrCat({"http://", text}) : text, ProxyCredentials::kRefuse);
  if (!endpoint.has_value() ||
      !(endpoint->server.is_http() || endpoint->server.is_https())) {
    return std::nullopt;
  }
  return endpoint->server.host_port_pair();
}

// The stored sign-in when it was written for `server`.
std::optional<ProxyAuthAnswer> SignInFor(const PrefService& prefs,
                                         const net::HostPortPair& server) {
  const PrefService::Preference* pref = prefs.FindPreference(kProxyAuthPref);
  if (!pref || !pref->GetValue()->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue& stored = pref->GetValue()->GetDict();
  const std::string* for_server = stored.FindString("server");
  const std::string* username = stored.FindString("username");
  const std::string* password = stored.FindString("password");
  if (!for_server || *for_server != server.ToString() || !username ||
      username->empty() || !password || password->empty()) {
    return std::nullopt;
  }
  return ProxyAuthAnswer{*username, *password};
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
  if (request.mode == kModeAutoDetect) {
    return std::optional<base::DictValue>(
        ProxyConfigDictionary::CreateAutoDetect());
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
    // The network service fetches a PAC script over http, https or data only.
    if (!url.SchemeIsHTTPOrHTTPS()) {
      return Refuse("pacUrl", "scheme");
    }
    return std::optional<base::DictValue>(
        ProxyConfigDictionary::CreatePacScript(url.spec(),
                                               /*pac_mandatory=*/false));
  }
  return Refuse("mode", "malformed");
}

base::expected<ProxyAuthChange, ProxySettingsProblem> ResolveProxyAuthChange(
    const ProxySettingsRequest& request,
    const base::DictValue& stored) {
  const std::string username(base::TrimWhitespaceASCII(
      request.username.value_or(""), base::TRIM_ALL));
  const std::string& password =
      request.password ? *request.password : base::EmptyString();
  ProxyAuthChange change;
  if (username.empty()) {
    if (!password.empty()) {
      return Refuse("username", "required");
    }
    return change;
  }
  if (request.mode != kModeFixed) {
    return Refuse("username", "malformed");
  }
  const auto endpoint = ParseProxyEndpoint(request.server.value_or(""),
                                           ProxyCredentials::kRefuse);
  if (!endpoint.has_value()) {
    return Refuse("server", ProxyEndpointProblemWireName(endpoint.error()));
  }
  // The SOCKS client offers no authentication.
  if (!(endpoint->server.is_http() || endpoint->server.is_https())) {
    return Refuse("username", "scheme");
  }
  if (username.size() > kMaxProxyAuthChars) {
    return Refuse("username", "tooLong");
  }
  // Basic sign-in joins the two with a colon.
  if (HasControlCharacter(username) ||
      username.find(':') != std::string::npos) {
    return Refuse("username", "malformed");
  }
  change.server = endpoint->server.host_port_pair().ToString();
  change.username = username;
  if (password.empty()) {
    const std::string* for_server = stored.FindString("server");
    const std::string* for_user = stored.FindString("username");
    const std::string* kept = stored.FindString("password");
    if (!for_server || *for_server != change.server || !for_user ||
        *for_user != username || !kept || kept->empty()) {
      return Refuse("password", "required");
    }
    change.kind = ProxyAuthChange::Kind::kKeep;
    return change;
  }
  if (password.size() > kMaxProxyAuthChars) {
    return Refuse("password", "tooLong");
  }
  if (HasControlCharacter(password)) {
    return Refuse("password", "malformed");
  }
  change.kind = ProxyAuthChange::Kind::kStore;
  change.password = password;
  return change;
}

std::optional<ProxyAuthAnswer> StoredProxyAuthFor(
    const PrefService& prefs,
    const net::HostPortPair& challenger) {
  const PrefService::Preference* pref =
      prefs.FindPreference(proxy_config::prefs::kProxy);
  if (!pref || !pref->IsUserControlled() || !pref->GetValue()->is_dict()) {
    return std::nullopt;
  }
  const ProxyConfigDictionary in_force(pref->GetValue()->GetDict().Clone());
  ProxyPrefs::ProxyMode mode = ProxyPrefs::MODE_SYSTEM;
  std::string text;
  if (!in_force.GetMode(&mode) || mode != ProxyPrefs::MODE_FIXED_SERVERS ||
      !in_force.GetProxyServer(&text)) {
    return std::nullopt;
  }
  const std::optional<net::HostPortPair> server = SignInProxyOf(text);
  if (!server || *server != challenger) {
    return std::nullopt;
  }
  return SignInFor(prefs, *server);
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
      if (const std::optional<net::HostPortPair> server = SignInProxyOf(text)) {
        if (const auto sign_in = SignInFor(prefs, *server)) {
          record.Set("username", sign_in->username);
          record.Set("hasPassword", true);
        }
      }
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
