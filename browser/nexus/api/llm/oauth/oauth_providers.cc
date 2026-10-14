// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"

#include "base/no_destructor.h"

namespace nexus {

OAuthProviderConfig::OAuthProviderConfig() = default;
OAuthProviderConfig::OAuthProviderConfig(const OAuthProviderConfig&) = default;
OAuthProviderConfig& OAuthProviderConfig::operator=(
    const OAuthProviderConfig&) = default;
OAuthProviderConfig::~OAuthProviderConfig() = default;

namespace {

// Builds the static provider table once. Reusing OpenAI's Codex CLI client
// is mandatory to reach the ChatGPT subscription backend — there is no
// public client for this.
const std::vector<OAuthProviderConfig>& GetAllOAuthProviders() {
  static const base::NoDestructor<std::vector<OAuthProviderConfig>> kProviders(
      [] {
        std::vector<OAuthProviderConfig> providers;

        OAuthProviderConfig chatgpt;
        chatgpt.id = "chatgpt-pro";
        chatgpt.display_name = "ChatGPT Plus/Pro";
        chatgpt.client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
        chatgpt.auth_endpoint = "https://auth.openai.com/oauth/authorize";
        chatgpt.token_endpoint = "https://auth.openai.com/oauth/token";
        chatgpt.scopes = {"openid", "profile", "email", "offline_access"};
        chatgpt.extra_auth_params = {
            {"id_token_add_organizations", "true"},
            {"codex_cli_simplified_flow", "true"},
            {"originator", "browseros"},
        };
        chatgpt.backend_base_url = "https://chatgpt.com/backend-api/codex";
        chatgpt.flow = OAuthFlow::kPkce;
        providers.push_back(std::move(chatgpt));

        // GitHub Copilot — device-code flow. GitHub's OAuth device/code and
        // access_token endpoints only parse application/x-www-form-urlencoded
        // request bodies (a JSON body silently drops client_id), so the device
        // flow MUST be form-encoded. The GitHub OAuth token returned here is
        // NOT a Copilot bearer: NexusOAuthManager exchanges it at
        // api.github.com/copilot_internal/v2/token for the short-lived token
        // that api.githubcopilot.com accepts (see EnsureCopilotToken).
        OAuthProviderConfig copilot;
        copilot.id = "github-copilot";
        copilot.display_name = "GitHub Copilot";
        copilot.client_id = "Ov23li8tweQw6odWQebz";
        copilot.auth_endpoint = "https://github.com/login/device/code";
        copilot.token_endpoint = "https://github.com/login/oauth/access_token";
        copilot.scopes = {"read:user"};
        copilot.backend_base_url = "https://api.githubcopilot.com";
        copilot.flow = OAuthFlow::kDeviceCode;
        copilot.device_code_use_form = true;        // form-urlencoded body
        copilot.device_code_requires_pkce = false;
        providers.push_back(std::move(copilot));

        // Qwen Code — device-code flow with PKCE; free OAuth quota routed to
        // the Qwen Code portal endpoint.
        OAuthProviderConfig qwen;
        qwen.id = "qwen-code";
        qwen.display_name = "Qwen Code";
        qwen.client_id = "f0304373b74a44d2b584a3fb70ca9e56";
        qwen.auth_endpoint = "https://chat.qwen.ai/api/v1/oauth2/device/code";
        qwen.token_endpoint = "https://chat.qwen.ai/api/v1/oauth2/token";
        qwen.scopes = {"openid", "profile", "email", "model.completion"};
        qwen.backend_base_url = "https://portal.qwen.ai/v1";
        qwen.flow = OAuthFlow::kDeviceCode;
        qwen.device_code_use_form = true;           // form-urlencoded body
        qwen.device_code_requires_pkce = true;
        providers.push_back(std::move(qwen));

        return providers;
      }());
  return *kProviders;
}

}  // namespace

const OAuthProviderConfig* GetOAuthProviderConfig(
    const std::string& provider_id) {
  for (const OAuthProviderConfig& config : GetAllOAuthProviders()) {
    if (config.id == provider_id) {
      return &config;
    }
  }
  return nullptr;
}

bool IsOAuthProvider(const std::string& provider_id) {
  return GetOAuthProviderConfig(provider_id) != nullptr;
}

}  // namespace nexus
