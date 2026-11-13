// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_SETTINGS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_SETTINGS_H_

#include <cstddef>
#include <optional>
#include <string>

#include "base/types/expected.h"
#include "base/values.h"

class PrefService;

namespace nexus {

// The global proxy setting behind `network.proxyGet` / `network.proxySet`:
// the profile's `proxy` preference, which every request on the profile's
// network context follows (F-27).

inline constexpr size_t kMaxProxyBypassListChars = 4096;

// What `network.proxySet` was asked to store.
struct ProxySettingsRequest {
  ProxySettingsRequest();
  ProxySettingsRequest(ProxySettingsRequest&&);
  ProxySettingsRequest& operator=(ProxySettingsRequest&&);
  ~ProxySettingsRequest();

  std::string mode;
  std::optional<std::string> server;
  std::optional<std::string> bypass_list;
  std::optional<std::string> pac_url;
};

// A refused request: the input at fault and the wire reason (`malformed`,
// `scheme`, `credentials`, `tooLong`).
struct ProxySettingsProblem {
  std::string field;
  std::string reason;
};

// Contract: the value to store in the `proxy` preference for `request`, or
// nullopt for `system` (the stored value is then removed). Pure.
base::expected<std::optional<base::DictValue>, ProxySettingsProblem>
BuildProxyPreference(const ProxySettingsRequest& request);

// Contract: the `proxy` preference in force as the wire record
// { mode, server?, bypassList?, pacUrl?, source, editable }. A fixed server
// stored without a scheme reads back as `http://…`.
base::DictValue DescribeProxyPreference(const PrefService& prefs);

// False when a store above the user's (a command-line switch, an extension,
// a policy) holds the `proxy` preference, or the preference is not registered.
bool IsProxyPreferenceEditable(const PrefService& prefs);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_SETTINGS_H_
