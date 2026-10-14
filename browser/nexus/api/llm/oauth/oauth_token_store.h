// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_TOKEN_STORE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_TOKEN_STORE_H_

#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"

class PrefService;

namespace nexus {

// OAuth tokens for a single provider, persisted inside the provider's
// `kNexusProviderConfigs[provider_id]` entry alongside the regular provider
// config (so the existing IsConfigured / catalog-enable plumbing keeps
// working). Tokens are stored in plaintext, matching how API keys are
// stored today.
struct OAuthTokens {
  OAuthTokens();
  OAuthTokens(const OAuthTokens&);
  OAuthTokens(OAuthTokens&&);
  OAuthTokens& operator=(const OAuthTokens&);
  OAuthTokens& operator=(OAuthTokens&&);
  ~OAuthTokens();

  std::string access_token;
  std::string refresh_token;
  // Unix epoch milliseconds at which `access_token` expires. 0 means
  // "unknown / does not expire" (treated as always-fresh).
  double expires_at_ms = 0;
  std::string account_id;
  std::string email;
  // Per-account API host returned by some providers (Qwen Code). When set,
  // SaveTokens normalizes it to `https://<host>/v1` and mirrors it into
  // `base_url` so the provider talks to the account's endpoint. Empty for
  // providers with a fixed backend.
  std::string resource_url;
};

// Thin read/write wrapper over the OAuth fields inside
// `kNexusProviderConfigs`. Does not own the PrefService.
class OAuthTokenStore {
 public:
  explicit OAuthTokenStore(PrefService* prefs);
  ~OAuthTokenStore();

  OAuthTokenStore(const OAuthTokenStore&) = delete;
  OAuthTokenStore& operator=(const OAuthTokenStore&) = delete;

  std::optional<OAuthTokens> GetTokens(const std::string& provider_id) const;

  // Merges `tokens` into the provider's config: writes the oauth_* fields,
  // mirrors the access token into `api_key` (so OpenAiProvider::Configure /
  // IsConfigured work unchanged), and stamps `last_verification` so the
  // catalog marks the provider enabled.
  void SaveTokens(const std::string& provider_id, const OAuthTokens& tokens);

  // Removes the entire provider config entry (used for "Disconnect").
  void DeleteTokens(const std::string& provider_id);

  bool HasTokens(const std::string& provider_id) const;

 private:
  raw_ptr<PrefService> prefs_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_TOKEN_STORE_H_
