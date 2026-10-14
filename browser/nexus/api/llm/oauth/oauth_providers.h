// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_PROVIDERS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_PROVIDERS_H_

#include <string>
#include <utility>
#include <vector>

namespace nexus {

// Loopback redirect that PKCE OAuth clients (currently the OpenAI Codex
// client) are registered against. The browser captures the navigation to
// this URL with a NavigationThrottle instead of binding a socket.
inline constexpr int kNexusOAuthCallbackPort = 1455;
inline constexpr char kNexusOAuthCallbackPath[] = "/auth/callback";
inline constexpr char kNexusOAuthRedirectUri[] =
    "http://localhost:1455/auth/callback";

// How the user authenticates with an OAuth provider.
enum class OAuthFlow {
  // Authorization-code + PKCE with a loopback redirect (ChatGPT Plus/Pro).
  kPkce,
  // RFC 8628 device-authorization grant: show a user code + verification URL,
  // then poll the token endpoint (GitHub Copilot, Qwen Code).
  kDeviceCode,
};

// Static description of an OAuth-backed LLM provider. Mirrors the BrowserOS
// `OAUTH_PROVIDERS` map.
struct OAuthProviderConfig {
  OAuthProviderConfig();
  OAuthProviderConfig(const OAuthProviderConfig&);
  OAuthProviderConfig& operator=(const OAuthProviderConfig&);
  ~OAuthProviderConfig();

  std::string id;
  std::string display_name;
  std::string client_id;
  // PKCE: authorization endpoint. Device-code: device-authorization endpoint.
  std::string auth_endpoint;
  std::string token_endpoint;
  std::vector<std::string> scopes;  // Joined with ' ' for the scope param.
  std::vector<std::pair<std::string, std::string>> extra_auth_params;
  // Base URL the LLM provider issues inference requests against once the
  // user is authenticated (e.g. the ChatGPT Codex backend).
  std::string backend_base_url;

  OAuthFlow flow = OAuthFlow::kPkce;
  // Device-code only: send request params as form-urlencoded (true) or JSON
  // (false), and whether the device-code request must carry a PKCE challenge.
  bool device_code_use_form = false;
  bool device_code_requires_pkce = false;
};

// Returns the config for `provider_id`, or nullptr if it is not an
// OAuth-backed provider.
const OAuthProviderConfig* GetOAuthProviderConfig(
    const std::string& provider_id);

// True when `provider_id` is an OAuth-backed provider.
bool IsOAuthProvider(const std::string& provider_id);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_PROVIDERS_H_
