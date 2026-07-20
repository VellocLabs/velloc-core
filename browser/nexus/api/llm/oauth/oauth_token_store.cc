// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_token_store.h"

#include <string_view>
#include <utility>

#include "base/time/time.h"
#include "base/values.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

namespace nexus {

OAuthTokens::OAuthTokens() = default;
OAuthTokens::OAuthTokens(const OAuthTokens&) = default;
OAuthTokens::OAuthTokens(OAuthTokens&&) = default;
OAuthTokens& OAuthTokens::operator=(const OAuthTokens&) = default;
OAuthTokens& OAuthTokens::operator=(OAuthTokens&&) = default;
OAuthTokens::~OAuthTokens() = default;

namespace {

constexpr char kAccessToken[] = "oauth_access_token";
constexpr char kRefreshToken[] = "oauth_refresh_token";
constexpr char kExpiresAt[] = "oauth_expires_at";
constexpr char kAccountId[] = "oauth_account_id";
constexpr char kEmail[] = "oauth_email";
constexpr char kResourceUrl[] = "oauth_resource_url";

// Normalizes a provider-supplied `resource_url` (typically a bare host like
// "portal.qwen.ai") into an OpenAI-compatible base URL "https://<host>/v1".
std::string NormalizeResourceUrl(const std::string& resource_url) {
  std::string base = resource_url;
  if (base.find("://") == std::string::npos) {
    base = "https://" + base;
  }
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  constexpr std::string_view kV1 = "/v1";
  if (base.size() < kV1.size() ||
      base.compare(base.size() - kV1.size(), kV1.size(), kV1) != 0) {
    base += "/v1";
  }
  return base;
}

}  // namespace

OAuthTokenStore::OAuthTokenStore(PrefService* prefs) : prefs_(prefs) {}
OAuthTokenStore::~OAuthTokenStore() = default;

std::optional<OAuthTokens> OAuthTokenStore::GetTokens(
    const std::string& provider_id) const {
  if (!prefs_) {
    return std::nullopt;
  }
  const base::DictValue& all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs);
  const base::DictValue* config = all_configs.FindDict(provider_id);
  if (!config) {
    return std::nullopt;
  }
  const std::string* access = config->FindString(kAccessToken);
  if (!access || access->empty()) {
    return std::nullopt;
  }

  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  OAuthTokens tokens;
  tokens.access_token = crypt->Decrypt(*access);
  if (tokens.access_token.empty()) {
    // Non-empty ciphertext decrypted to nothing — the OS encryptor isn't
    // ready yet (cold-boot window). Without this guard, IsConnected() reports
    // true and EnsureFreshToken hands the provider an EMPTY api_key with
    // ok=true, so the first requests after startup 401 confusingly. Treat
    // "can't decrypt yet" as "no tokens yet"; callers retry naturally once
    // the encryptor arrives.
    return std::nullopt;
  }
  if (const std::string* refresh = config->FindString(kRefreshToken)) {
    tokens.refresh_token = crypt->Decrypt(*refresh);
  }
  tokens.expires_at_ms = config->FindDouble(kExpiresAt).value_or(0);
  if (const std::string* account = config->FindString(kAccountId)) {
    tokens.account_id = *account;
  }
  if (const std::string* email = config->FindString(kEmail)) {
    tokens.email = *email;
  }
  if (const std::string* resource_url = config->FindString(kResourceUrl)) {
    tokens.resource_url = *resource_url;
  }
  return tokens;
}

void OAuthTokenStore::SaveTokens(const std::string& provider_id,
                                 const OAuthTokens& tokens) {
  if (!prefs_) {
    return;
  }
  base::DictValue all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs).Clone();
  base::DictValue* config = all_configs.EnsureDict(provider_id);

  // Encrypt the bearer secrets at rest (access + refresh token, and the
  // `api_key` mirror below). The provider's Configure() + GetTokens()
  // decrypt on read; account_id / email / resource_url are identifiers,
  // not bearer secrets, and stay plaintext.
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  config->Set(kAccessToken, crypt->Encrypt(tokens.access_token));
  config->Set(kRefreshToken, crypt->Encrypt(tokens.refresh_token));
  config->Set(kExpiresAt, tokens.expires_at_ms);
  config->Set(kAccountId, tokens.account_id);
  config->Set(kEmail, tokens.email);
  config->Set(kResourceUrl, tokens.resource_url);

  // Mirror the access token into `api_key` and the account id into
  // `account_id` so OpenAiProvider::Configure() picks both up when the
  // provider is reconfigured from prefs (login reconfigure / browser restart)
  // — without these mirrors, IsConfigured() and the ChatGPT-Account-Id header
  // would be empty until the next inference call.
  config->Set("api_key", crypt->Encrypt(tokens.access_token));
  config->Set("account_id", tokens.account_id);

  // Providers that return a per-account API host (Qwen Code) point the
  // provider at it via the regular `base_url` config key.
  if (!tokens.resource_url.empty()) {
    config->Set("base_url", NormalizeResourceUrl(tokens.resource_url));
  }

  // Stamp last_verification so NexusLlmHost::BuildCatalog marks the provider
  // enabled (it has no /v1/models verify step of its own).
  base::DictValue verification;
  verification.Set("providerId", provider_id);
  verification.Set("verifiedAtMs",
                   base::Time::Now().InMillisecondsFSinceUnixEpoch());
  config->Set("last_verification", std::move(verification));

  prefs_->SetDict(prefs::kNexusProviderConfigs, std::move(all_configs));
}

void OAuthTokenStore::DeleteTokens(const std::string& provider_id) {
  if (!prefs_) {
    return;
  }
  base::DictValue all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs).Clone();
  all_configs.Remove(provider_id);
  prefs_->SetDict(prefs::kNexusProviderConfigs, std::move(all_configs));
}

bool OAuthTokenStore::HasTokens(const std::string& provider_id) const {
  return GetTokens(provider_id).has_value();
}

}  // namespace nexus
