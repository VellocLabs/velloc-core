// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_PROVIDER_OAUTH_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_PROVIDER_OAUTH_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"

class Profile;

namespace nexus {

// The `{{oauth:NAME}}` placeholders (llm_raw_stream_policy.h) need the
// provider's backend-ready OAuth values — and getting them means running the
// host's refresh / exchange step first, exactly as the C++ providers do before
// every call: NexusOAuthManager::EnsureFreshToken refreshes an expiring
// ChatGPT / Qwen token and exchanges the GitHub token for a Copilot session
// token. Shared by `model.rawStreamOpen` and the `network.httpRequest` /
// `network.streamOpen` providerId form.
using ProviderOAuthCallback =
    base::OnceCallback<void(std::optional<OAuthValues> values,
                            std::string error)>;

// Resolves `provider_id`'s values. Fails (nullopt + a message) when the
// provider is not OAuth-backed, the profile has no broker, or the refresh /
// exchange failed (not signed in). UI thread; `callback` runs on UI, possibly
// synchronously.
void ResolveProviderOAuthValues(Profile* profile,
                                const std::string& provider_id,
                                ProviderOAuthCallback callback);

// The base URL a provider's requests are anchored to: the live provider's
// active URL (user override, else its default), else the `base_url` in its
// stored config. Empty when the provider is unknown.
std::string GetProviderAnchorBaseUrl(Profile* profile,
                                     const std::string& provider_id);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_PROVIDER_OAUTH_H_
