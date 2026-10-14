// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/secrets/nexus_secret_store.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "net/base/url_util.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace nexus {

namespace {

constexpr std::string_view kPlaceholderOpen = "{{secret:";
constexpr std::string_view kPlaceholderClose = "}}";
constexpr size_t kMaxNameRestChars = 128;

constexpr const char* kNamespaces[] = {"provider", "plugin", "mcp"};

bool IsNameChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '.' || c == ':' ||
         c == '/' || c == '@' || c == '-';
}

constexpr char kValueKey[] = "value";
constexpr char kOriginKey[] = "origin";
constexpr char kProcessEnvKey[] = "process_env";

constexpr const char* kProcessEnvNamespaces[] = {"plugin", "mcp"};

// The sealed value of `entry`, opened: kFound with the plaintext, or
// kUnavailable when the keystore is not ready / the value will not open.
SecretLookupResult Open(const std::string& sealed) {
  SecretLookupResult result;
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  if (!crypt->ready()) {
    result.status = SecretLookupResult::Status::kUnavailable;
    return result;
  }
  std::string plaintext = crypt->Decrypt(sealed);
  if (plaintext.empty()) {
    result.status = SecretLookupResult::Status::kUnavailable;
    return result;
  }
  result.status = SecretLookupResult::Status::kFound;
  result.value = std::move(plaintext);
  return result;
}

}  // namespace

bool IsValidNamespacedSecretName(std::string_view name) {
  const size_t colon = name.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const std::string_view name_space = name.substr(0, colon);
  const std::string_view rest = name.substr(colon + 1);
  if (std::ranges::find(kNamespaces, name_space) == std::end(kNamespaces)) {
    return false;
  }
  if (rest.empty() || rest.size() > kMaxNameRestChars) {
    return false;
  }
  return std::ranges::all_of(rest, IsNameChar);
}

bool SecretNamespaceAllowsProcessEnv(std::string_view name_space) {
  return std::ranges::find(kProcessEnvNamespaces, name_space) !=
         std::end(kProcessEnvNamespaces);
}

std::string_view SecretNamespace(std::string_view name) {
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string_view()
                                         : name.substr(0, colon);
}

std::optional<std::string> CanonicalSecretOrigin(std::string_view origin) {
  const GURL url{std::string(origin)};
  if (!url.is_valid() || url.has_username() || url.has_password()) {
    return std::nullopt;
  }
  if (!url.SchemeIs(url::kHttpsScheme) &&
      !(url.SchemeIs(url::kHttpScheme) && net::IsLocalhost(url))) {
    return std::nullopt;
  }
  const url::Origin canonical = url::Origin::Create(url);
  if (canonical.opaque()) {
    return std::nullopt;
  }
  return canonical.Serialize();
}

bool HasSecretPlaceholder(std::string_view text) {
  return text.find(kPlaceholderOpen) != std::string_view::npos;
}

std::optional<SecretExpansionError> ExpandSecretPlaceholders(
    std::string_view text,
    const NamespacedSecretLookup& lookup,
    std::string* out) {
  out->clear();
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t open = text.find(kPlaceholderOpen, pos);
    if (open == std::string_view::npos) {
      out->append(text.substr(pos));
      break;
    }
    out->append(text.substr(pos, open - pos));
    const size_t name_start = open + kPlaceholderOpen.size();
    const size_t close = text.find(kPlaceholderClose, name_start);
    if (close == std::string_view::npos) {
      return SecretExpansionError{"INVALID_ARGS",
                                  "Unterminated {{secret:…}} placeholder."};
    }
    const std::string name(text.substr(name_start, close - name_start));
    if (!IsValidNamespacedSecretName(name)) {
      return SecretExpansionError{
          "INVALID_ARGS", "Invalid secret name in placeholder: " + name};
    }
    const SecretLookupResult found = lookup.Run(name);
    switch (found.status) {
      case SecretLookupResult::Status::kFound:
        out->append(found.value);
        break;
      case SecretLookupResult::Status::kOriginMismatch:
        return SecretExpansionError{
            "SECRET_ORIGIN_MISMATCH",
            "Secret " + name + " is bound to another origin."};
      case SecretLookupResult::Status::kUnavailable:
        return SecretExpansionError{
            "SECRET_UNAVAILABLE",
            "The OS keystore is not ready; secret " + name +
                " cannot be read."};
      case SecretLookupResult::Status::kMissing:
        return SecretExpansionError{"SECRET_UNAVAILABLE",
                                    "No secret named " + name + "."};
      case SecretLookupResult::Status::kNotPermitted:
        return SecretExpansionError{
            "SECRET_NOT_PERMITTED",
            "Secret " + name + " was not stored for process use."};
    }
    pos = close + kPlaceholderClose.size();
  }
  return std::nullopt;
}

NexusSecretStore::NexusSecretStore(PrefService* prefs) : prefs_(prefs) {}

NexusSecretStore::~NexusSecretStore() = default;

std::optional<SecretExpansionError> NexusSecretStore::Set(
    const std::string& name,
    const std::string& value,
    const std::string& origin,
    bool process_env) {
  if (!IsValidNamespacedSecretName(name)) {
    return SecretExpansionError{
        "INVALID_ARGS",
        "A secret name is <provider|plugin|mcp>:<1-128 of [A-Za-z0-9_.:/@-]>."};
  }
  if (value.empty() || value.size() > kMaxValueBytes) {
    return SecretExpansionError{"INVALID_ARGS",
                                "A secret value is 1 to 16384 bytes."};
  }
  if (process_env && !SecretNamespaceAllowsProcessEnv(SecretNamespace(name))) {
    return SecretExpansionError{
        "INVALID_ARGS", "Only plugin: and mcp: secrets can be for process use."};
  }
  // A process-only secret has no origin: no network request can use it.
  std::optional<std::string> bound =
      process_env && origin.empty() ? std::make_optional(std::string())
                                    : CanonicalSecretOrigin(origin);
  if (!bound) {
    return SecretExpansionError{
        "INVALID_ARGS",
        "A secret's origin is an https URL (http only for localhost)."};
  }
  if (!prefs_) {
    return SecretExpansionError{"SECRET_STORE_UNAVAILABLE",
                                "No profile to store the secret in."};
  }
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  if (!crypt->ready()) {
    return SecretExpansionError{"SECRET_STORE_UNAVAILABLE",
                                "The OS keystore is not ready."};
  }
  std::string sealed = crypt->Encrypt(value);
  // Encrypt degrades to the plaintext when it cannot seal; never store that.
  if (!NexusSecretCrypt::IsEncrypted(sealed)) {
    return SecretExpansionError{"SECRET_STORE_UNAVAILABLE",
                                "The OS keystore could not seal the secret."};
  }

  ScopedDictPrefUpdate update(prefs_, prefs::kNexusSecrets);
  if (!update->contains(name) && update->size() >= kMaxSecrets) {
    return SecretExpansionError{"TOO_MANY_SECRETS",
                                "The secret store is full (512 secrets)."};
  }
  base::DictValue entry;
  entry.Set(kValueKey, std::move(sealed));
  entry.Set(kOriginKey, std::move(*bound));
  if (process_env) {
    entry.Set(kProcessEnvKey, true);
  }
  update->Set(name, std::move(entry));
  return std::nullopt;
}

std::optional<std::string> NexusSecretStore::Has(
    const std::string& name) const {
  if (!prefs_) {
    return std::nullopt;
  }
  const base::DictValue* entry =
      prefs_->GetDict(prefs::kNexusSecrets).FindDict(name);
  if (!entry) {
    return std::nullopt;
  }
  const std::string* origin = entry->FindString(kOriginKey);
  return origin ? *origin : std::string();
}

bool NexusSecretStore::Delete(const std::string& name) {
  if (!prefs_ || !prefs_->GetDict(prefs::kNexusSecrets).contains(name)) {
    return false;
  }
  ScopedDictPrefUpdate update(prefs_, prefs::kNexusSecrets);
  return update->Remove(name);
}

std::vector<NexusSecretStore::Entry> NexusSecretStore::ListNames(
    const std::optional<std::string>& name_space) const {
  std::vector<Entry> out;
  if (!prefs_) {
    return out;
  }
  for (const auto [name, value] : prefs_->GetDict(prefs::kNexusSecrets)) {
    if (name_space.has_value() && SecretNamespace(name) != *name_space) {
      continue;
    }
    const base::DictValue* entry = value.GetIfDict();
    const std::string* origin = entry ? entry->FindString(kOriginKey) : nullptr;
    out.push_back(Entry{
        name, origin ? *origin : std::string(),
        entry && entry->FindBool(kProcessEnvKey).value_or(false)});
  }
  std::ranges::sort(out, {}, &Entry::name);
  return out;
}

SecretLookupResult NexusSecretStore::Lookup(
    const std::string& name,
    const url::Origin& request_origin) const {
  SecretLookupResult result;
  if (!prefs_) {
    result.status = SecretLookupResult::Status::kMissing;
    return result;
  }
  const base::DictValue* entry =
      prefs_->GetDict(prefs::kNexusSecrets).FindDict(name);
  const std::string* sealed = entry ? entry->FindString(kValueKey) : nullptr;
  const std::string* origin = entry ? entry->FindString(kOriginKey) : nullptr;
  if (!sealed || !origin) {
    result.status = SecretLookupResult::Status::kMissing;
    return result;
  }
  // A process-only secret's empty origin never matches a serialized one.
  if (request_origin.opaque() || origin->empty() ||
      request_origin.Serialize() != *origin) {
    result.status = SecretLookupResult::Status::kOriginMismatch;
    return result;
  }
  return Open(*sealed);
}

SecretLookupResult NexusSecretStore::LookupForProcessEnv(
    const std::string& name) const {
  SecretLookupResult result;
  const base::DictValue* entry =
      prefs_ ? prefs_->GetDict(prefs::kNexusSecrets).FindDict(name) : nullptr;
  const std::string* sealed = entry ? entry->FindString(kValueKey) : nullptr;
  if (!sealed) {
    result.status = SecretLookupResult::Status::kMissing;
    return result;
  }
  // The namespace is re-checked so a pref written any other way cannot
  // widen it.
  if (!entry->FindBool(kProcessEnvKey).value_or(false) ||
      !SecretNamespaceAllowsProcessEnv(SecretNamespace(name))) {
    result.status = SecretLookupResult::Status::kNotPermitted;
    return result;
  }
  return Open(*sealed);
}

NamespacedSecretLookup NexusSecretStore::ProcessEnvLookup() const {
  return base::BindRepeating(
      [](const NexusSecretStore* store, const std::string& name) {
        return store->LookupForProcessEnv(name);
      },
      base::Unretained(this));
}

NamespacedSecretLookup NexusSecretStore::LookupFor(
    const url::Origin& request_origin) const {
  return base::BindRepeating(
      [](const NexusSecretStore* store, const url::Origin& origin,
         const std::string& name) { return store->Lookup(name, origin); },
      base::Unretained(this), request_origin);
}

}  // namespace nexus
