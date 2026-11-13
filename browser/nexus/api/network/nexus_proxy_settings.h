// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_SETTINGS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_SETTINGS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/types/expected.h"
#include "base/values.h"

class PrefService;

namespace url {
class SchemeHostPort;
}

namespace nexus {

// The global proxy setting behind `network.proxyGet` / `network.proxySet`:
// the profile's `proxy` preference, which every request on the profile's
// network context follows (F-18).

inline constexpr size_t kMaxProxyBypassListChars = 4096;
inline constexpr size_t kMaxProxyAuthChars = 255;

// The profile preference holding the fixed proxy's sign-in:
// { server: "scheme://host:port", username, password }. `password` is sealed by the
// writer (NexusSecretCrypt); this module never sees it in the clear.
inline constexpr char kProxyAuthPref[] = "nexus.proxy_auth";

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
  // With `fixed`: the sign-in an http or https proxy asks for.
  std::optional<std::string> username;
  std::optional<std::string> password;
};

// A refused request: the input at fault and the wire reason (`malformed`,
// `scheme`, `credentials`, `tooLong`, `required`).
struct ProxySettingsProblem {
  std::string field;
  std::string reason;
};

// Contract: the value to store in the `proxy` preference for `request`, or
// nullopt for `system` (the stored value is then removed). Pure.
base::expected<std::optional<base::DictValue>, ProxySettingsProblem>
BuildProxyPreference(const ProxySettingsRequest& request);

// What a request does to the stored sign-in.
struct ProxyAuthChange {
  enum class Kind {
    kClear,  // No sign-in goes with the new setting.
    kStore,  // Store `server`, `username` and `password`.
    kKeep,   // The stored sign-in still applies as it is.
  };
  Kind kind = Kind::kClear;
  std::string server;  // scheme://host:port
  std::string username;
  std::string password;  // In the clear; the caller seals it.
};

// Contract: `request` must already build a preference. A user name goes
// with `fixed` and an http or https proxy only (`username:scheme`,
// `username:malformed`). A request with a user name and no password keeps
// the stored password when `stored` (the kProxyAuthPref value) is for the
// same proxy and user name, and is refused `password:required` otherwise,
// so a stored password is never carried over to another proxy. Pure.
base::expected<ProxyAuthChange, ProxySettingsProblem> ResolveProxyAuthChange(
    const ProxySettingsRequest& request,
    const base::DictValue& stored);

// The stored sign-in for a proxy that asked for one.
struct ProxyAuthAnswer {
  std::string username;
  std::string sealed_password;
};

// Security: answers only when the `proxy` preference in force is the
// user's own, names one fixed http or https proxy, that proxy is
// `challenger` (scheme, host and port), and the stored sign-in was written
// for it.
std::optional<ProxyAuthAnswer> StoredProxyAuthFor(
    const PrefService& prefs,
    const url::SchemeHostPort& challenger);

// Contract: the `proxy` preference in force as the wire record
// { mode, server?, bypassList?, pacUrl?, username?, hasPassword?, source,
// editable }. A fixed server stored without a scheme reads back as
// `http://…`. `username` and `hasPassword` appear only under the user's own
// setting. The password is never part of the record.
base::DictValue DescribeProxyPreference(const PrefService& prefs);

// False when a store above the user's (a command-line switch, an extension,
// a policy) holds the `proxy` preference, or the preference is not registered.
bool IsProxyPreferenceEditable(const PrefService& prefs);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_SETTINGS_H_
