// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_provider_oauth.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"

namespace nexus {

void ResolveProviderOAuthValues(Profile* profile,
                                const std::string& provider_id,
                                ProviderOAuthCallback callback) {
  if (!IsOAuthProvider(provider_id)) {
    std::move(callback).Run(std::nullopt,
                            provider_id + " is not an OAuth provider.");
    return;
  }
  LlmBroker* broker = profile ? LlmBrokerFactory::GetForProfile(profile)
                              : nullptr;
  NexusOAuthManager* oauth = broker ? broker->oauth_manager() : nullptr;
  if (!oauth) {
    std::move(callback).Run(std::nullopt, "OAuth is not available.");
    return;
  }
  oauth->EnsureFreshToken(
      provider_id,
      base::BindOnce(
          [](ProviderOAuthCallback callback, std::string provider_id, bool ok,
             std::string access_token, std::string account_id) {
            if (!ok || access_token.empty()) {
              std::move(callback).Run(
                  std::nullopt, provider_id +
                                    " is not signed in, or its token could "
                                    "not be refreshed.");
              return;
            }
            OAuthValues values;
            values.access_token = std::move(access_token);
            values.account_id = std::move(account_id);
            std::move(callback).Run(std::move(values), std::string());
          },
          std::move(callback), provider_id));
}

std::string GetProviderAnchorBaseUrl(Profile* profile,
                                     const std::string& provider_id) {
  if (!profile) {
    return std::string();
  }
  if (LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile)) {
    if (LlmProvider* provider = broker->GetProvider(provider_id)) {
      return provider->GetBaseUrl();
    }
  }
  if (const base::DictValue* config =
          GetStoredProviderConfig(profile->GetPrefs(), provider_id)) {
    if (const std::string* base_url = config->FindString("base_url")) {
      return *base_url;
    }
  }
  return std::string();
}

}  // namespace nexus
