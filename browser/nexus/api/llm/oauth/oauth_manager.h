// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_MANAGER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_MANAGER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_token_store.h"

class PrefService;

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace nexus {

// Owns the OAuth login lifecycle (PKCE authorization, token exchange, and
// refresh) for subscription-backed LLM providers. One instance per profile,
// owned by LlmBroker. Token persistence is delegated to OAuthTokenStore;
// in-memory provider reconfiguration is the caller's responsibility (this
// class deliberately has no dependency on the broker/provider layer).
class NexusOAuthManager {
 public:
  // Invoked when an authorization-code exchange (the redirect callback)
  // completes. `error` is empty on success.
  using AuthCodeCallback =
      base::OnceCallback<void(bool ok, std::string error)>;

  // Invoked by EnsureFreshToken with a usable access token (refreshed if it
  // was near expiry). On failure `ok` is false and the strings are empty.
  using EnsureTokenCallback =
      base::OnceCallback<void(bool ok,
                              std::string access_token,
                              std::string account_id)>;

  // `on_tokens_saved` is invoked (with the provider id) right after tokens are
  // persisted by a login or refresh, so the owner can reconfigure the matching
  // in-memory provider from prefs — otherwise the provider's IsConfigured()
  // stays false until the next browser start and the model catalog hides it.
  NexusOAuthManager(
      PrefService* prefs,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      base::RepeatingCallback<void(const std::string& provider_id)>
          on_tokens_saved);
  ~NexusOAuthManager();

  NexusOAuthManager(const NexusOAuthManager&) = delete;
  NexusOAuthManager& operator=(const NexusOAuthManager&) = delete;

  // Begins a PKCE login for `provider_id`. Records the pending flow keyed by
  // the generated `state` and returns the authorization URL the caller should
  // open in a tab. Returns an empty string if `provider_id` is unknown.
  std::string BeginLogin(const std::string& provider_id);

  // User-facing info for a device-authorization grant: the code to enter and
  // the URL to enter it at.
  struct DeviceCodeInfo {
    DeviceCodeInfo();
    DeviceCodeInfo(const DeviceCodeInfo&);
    DeviceCodeInfo& operator=(const DeviceCodeInfo&);
    ~DeviceCodeInfo();

    std::string user_code;
    std::string verification_uri;
    int expires_in = 0;
  };
  using DeviceCodeCallback =
      base::OnceCallback<void(bool ok, DeviceCodeInfo info, std::string error)>;

  // Begins a device-code login for `provider_id`: requests a device code and
  // returns the user code + verification URL via `callback`, then polls the
  // token endpoint in the background until the user authorizes (tokens are
  // saved + on_tokens_saved fired) or the code expires.
  void StartDeviceCodeFlow(const std::string& provider_id,
                           DeviceCodeCallback callback);

  // Exchanges an authorization `code` for tokens, matching the pending flow by
  // `state`. Called by the navigation throttle that captures the loopback
  // redirect.
  void HandleAuthCode(const std::string& code,
                      const std::string& state,
                      AuthCodeCallback callback);

  // Returns a fresh access token for `provider_id`, refreshing it first if it
  // is at/near expiry. The caller applies the returned token to the provider
  // (e.g. via Configure) before issuing the inference request.
  void EnsureFreshToken(const std::string& provider_id,
                        EnsureTokenCallback callback);

  bool IsConnected(const std::string& provider_id) const;
  std::string GetConnectedEmail(const std::string& provider_id) const;

  OAuthTokenStore* token_store() { return &store_; }

 private:
  struct PendingFlow {
    PendingFlow();
    PendingFlow(const PendingFlow&);
    PendingFlow(PendingFlow&&);
    PendingFlow& operator=(const PendingFlow&);
    PendingFlow& operator=(PendingFlow&&);
    ~PendingFlow();

    std::string provider_id;
    std::string code_verifier;
    base::Time created_at;
  };

  // Parsed subset of an OAuth token endpoint response.
  struct TokenResponse {
    TokenResponse();
    TokenResponse(const TokenResponse&);
    TokenResponse(TokenResponse&&);
    TokenResponse& operator=(const TokenResponse&);
    TokenResponse& operator=(TokenResponse&&);
    ~TokenResponse();

    std::string access_token;
    std::string refresh_token;
    int expires_in = 0;
    std::string id_token;
    // Per-account API host returned by some providers (Qwen Code) telling the
    // client which endpoint to call. Empty for providers with a fixed backend.
    std::string resource_url;
  };

  using TokenRequestCallback =
      base::OnceCallback<void(std::optional<TokenResponse>)>;

  void PostTokenRequest(const std::string& token_endpoint,
                        const std::string& form_body,
                        TokenRequestCallback callback);
  void OnTokenResponse(const std::string& loader_id,
                       TokenRequestCallback callback,
                       std::optional<std::string> response_body);

  // --- Device-code flow ---
  struct DeviceFlowState {
    DeviceFlowState();
    DeviceFlowState(const DeviceFlowState&);
    DeviceFlowState& operator=(const DeviceFlowState&);
    ~DeviceFlowState();

    std::string provider_id;
    std::string device_code;
    std::string code_verifier;  // empty unless the provider requires PKCE
    base::TimeTicks deadline;
    base::TimeDelta interval;
  };

  // Generic POST that returns the parsed JSON object (or nullopt on network /
  // parse failure). Used by the device-code requests, which need both JSON and
  // form bodies and must inspect non-token responses (authorization_pending).
  using JsonResponseCallback =
      base::OnceCallback<void(std::optional<base::DictValue>)>;
  void PostForJson(const std::string& url,
                   bool use_form,
                   const std::string& body,
                   JsonResponseCallback callback);
  // GET that returns the parsed JSON object. Used for the GitHub Copilot token
  // exchange (api.github.com/copilot_internal/v2/token).
  void GetForJson(
      const std::string& url,
      const std::vector<std::pair<std::string, std::string>>& headers,
      JsonResponseCallback callback);
  void OnPostForJson(const std::string& loader_id,
                     JsonResponseCallback callback,
                     std::optional<std::string> response_body);

  // Maps a provider's durable credential to a backend-ready access token. For
  // most providers this is the identity (the stored token is used directly);
  // for github-copilot it exchanges the GitHub OAuth token for the short-lived
  // Copilot token that api.githubcopilot.com accepts.
  void DeliverBackendToken(const std::string& provider_id,
                           const std::string& durable_token,
                           const std::string& account_id,
                           EnsureTokenCallback callback);
  void EnsureCopilotToken(const std::string& github_token,
                          EnsureTokenCallback callback);
  void OnCopilotTokenFetched(const std::string& github_token,
                             EnsureTokenCallback callback,
                             std::optional<base::DictValue> response);

  void OnDeviceCodeStarted(const std::string& provider_id,
                           const std::string& code_verifier,
                           DeviceCodeCallback callback,
                           std::optional<base::DictValue> response);
  void PollDeviceCode(DeviceFlowState state);
  void OnDeviceCodePoll(DeviceFlowState state,
                        std::optional<base::DictValue> response);

  void OnAuthCodeExchanged(const std::string& provider_id,
                           AuthCodeCallback callback,
                           std::optional<TokenResponse> response);

  void OnRefreshComplete(const std::string& provider_id,
                         std::optional<TokenResponse> response);
  void RunRefreshWaiters(const std::string& provider_id,
                         bool ok,
                         const std::string& access_token,
                         const std::string& account_id);

  void CleanExpiredFlows();

  raw_ptr<PrefService> prefs_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  base::RepeatingCallback<void(const std::string& provider_id)>
      on_tokens_saved_;
  OAuthTokenStore store_;

  std::map<std::string, PendingFlow> pending_flows_;  // keyed by state
  std::map<std::string, std::unique_ptr<network::SimpleURLLoader>> loaders_;
  // Callbacks waiting on an in-flight refresh, keyed by provider id.
  std::map<std::string, std::vector<EnsureTokenCallback>> refresh_waiters_;

  // Cached GitHub Copilot session token. The Copilot bearer is short-lived
  // (~30 min) and minted from the durable GitHub OAuth token, so it lives in
  // memory rather than prefs. `copilot_token_source_` records which GitHub
  // token it was derived from so a re-login invalidates the cache.
  std::string copilot_access_token_;
  std::string copilot_token_source_;
  base::Time copilot_token_expiry_;

  base::WeakPtrFactory<NexusOAuthManager> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_MANAGER_H_
