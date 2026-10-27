// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"

#include <utility>

#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "base/values.h"
#include "crypto/random.h"
#include "crypto/sha2.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr base::TimeDelta kFlowTtl = base::Minutes(10);
// Refresh slightly early so a request never ships an about-to-expire token.
constexpr base::TimeDelta kExpiryBuffer = base::Seconds(60);
constexpr int kMaxResponseSizeBytes = 1 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_oauth_token", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Exchanges or refreshes OAuth tokens with the LLM provider's "
          "authorization server so the user can power the Nexus agent with "
          "their existing subscription (e.g. ChatGPT Plus/Pro) instead of an "
          "API key."
        trigger:
          "User clicks 'Sign in' for an OAuth-backed provider in Nexus "
          "settings, or an inference request needs a refreshed access token."
        data:
          "OAuth authorization code or refresh token, PKCE code verifier, and "
          "the registered client id. No conversation content is sent."
        destination: OTHER
        internal {
          contacts { email: "nicke@nicke.dev" }
        }
        user_data {
          type: ACCESS_TOKEN
        }
        last_reviewed: "2026-05-20"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users opt in by signing into an OAuth provider in the Nexus AI "
          "settings. No requests are sent without an explicit sign-in."
        policy_exception_justification:
          "User-initiated authentication for an explicitly opted-in feature."
      })");

std::string FormEscape(std::string_view value) {
  return base::EscapeUrlEncodedData(value, /*use_plus=*/true);
}

// Encodes `params` as either an application/x-www-form-urlencoded string or a
// flat JSON object, per the provider's device-code content type.
std::string BuildBody(
    const std::vector<std::pair<std::string, std::string>>& params,
    bool use_form) {
  if (use_form) {
    std::string out;
    for (const auto& [key, value] : params) {
      if (!out.empty()) {
        out += '&';
      }
      out += FormEscape(key) + "=" + FormEscape(value);
    }
    return out;
  }
  base::DictValue dict;
  for (const auto& [key, value] : params) {
    dict.Set(key, value);
  }
  std::string json;
  base::JSONWriter::Write(base::Value(std::move(dict)), &json);
  return json;
}

std::string GenerateRandomBase64Url(size_t num_bytes) {
  std::vector<uint8_t> bytes(num_bytes);
  crypto::RandBytes(bytes);
  std::string out;
  base::Base64UrlEncode(bytes, base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
  return out;
}

std::string ComputeS256Challenge(const std::string& verifier) {
  const std::string digest = crypto::SHA256HashString(verifier);
  std::string out;
  base::Base64UrlEncode(digest, base::Base64UrlEncodePolicy::OMIT_PADDING,
                        &out);
  return out;
}

// Decodes the (unverified) JWT payload and pulls out the ChatGPT account id
// and email. Safe because the token comes straight from the provider's HTTPS
// token endpoint; do not reuse for caller-supplied tokens.
void ExtractClaims(const std::string& jwt,
                   std::string* account_id,
                   std::string* email) {
  const std::vector<std::string_view> parts = base::SplitStringPiece(
      jwt, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() != 3) {
    return;
  }
  std::string decoded;
  if (!base::Base64UrlDecode(
          parts[1], base::Base64UrlDecodePolicy::IGNORE_PADDING, &decoded)) {
    return;
  }
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(decoded, base::JSON_PARSE_RFC);
  if (!payload) {
    return;
  }

  if (const base::DictValue* auth =
          payload->FindDict("https://api.openai.com/auth")) {
    if (const std::string* a = auth->FindString("chatgpt_account_id")) {
      *account_id = *a;
    }
  }
  if (account_id->empty()) {
    if (const std::string* a = payload->FindString("chatgpt_account_id")) {
      *account_id = *a;
    }
  }
  if (account_id->empty()) {
    if (const std::string* a = payload->FindString("account_id")) {
      *account_id = *a;
    }
  }

  if (const base::DictValue* profile =
          payload->FindDict("https://api.openai.com/profile")) {
    if (const std::string* e = profile->FindString("email")) {
      *email = *e;
    }
  }
  if (email->empty()) {
    if (const std::string* e = payload->FindString("email")) {
      *email = *e;
    }
  }
}

}  // namespace

NexusOAuthManager::PendingFlow::PendingFlow() = default;
NexusOAuthManager::PendingFlow::PendingFlow(const PendingFlow&) = default;
NexusOAuthManager::PendingFlow::PendingFlow(PendingFlow&&) = default;
NexusOAuthManager::PendingFlow& NexusOAuthManager::PendingFlow::operator=(
    const PendingFlow&) = default;
NexusOAuthManager::PendingFlow& NexusOAuthManager::PendingFlow::operator=(
    PendingFlow&&) = default;
NexusOAuthManager::PendingFlow::~PendingFlow() = default;

NexusOAuthManager::TokenResponse::TokenResponse() = default;
NexusOAuthManager::TokenResponse::TokenResponse(const TokenResponse&) = default;
NexusOAuthManager::TokenResponse::TokenResponse(TokenResponse&&) = default;
NexusOAuthManager::TokenResponse& NexusOAuthManager::TokenResponse::operator=(
    const TokenResponse&) = default;
NexusOAuthManager::TokenResponse& NexusOAuthManager::TokenResponse::operator=(
    TokenResponse&&) = default;
NexusOAuthManager::TokenResponse::~TokenResponse() = default;

NexusOAuthManager::DeviceCodeInfo::DeviceCodeInfo() = default;
NexusOAuthManager::DeviceCodeInfo::DeviceCodeInfo(const DeviceCodeInfo&) =
    default;
NexusOAuthManager::DeviceCodeInfo& NexusOAuthManager::DeviceCodeInfo::operator=(
    const DeviceCodeInfo&) = default;
NexusOAuthManager::DeviceCodeInfo::~DeviceCodeInfo() = default;

NexusOAuthManager::DeviceFlowState::DeviceFlowState() = default;
NexusOAuthManager::DeviceFlowState::DeviceFlowState(const DeviceFlowState&) =
    default;
NexusOAuthManager::DeviceFlowState& NexusOAuthManager::DeviceFlowState::
operator=(const DeviceFlowState&) = default;
NexusOAuthManager::DeviceFlowState::~DeviceFlowState() = default;

NexusOAuthManager::DeviceFlowSlot::DeviceFlowSlot() = default;
NexusOAuthManager::DeviceFlowSlot::DeviceFlowSlot(DeviceFlowSlot&&) = default;
NexusOAuthManager::DeviceFlowSlot& NexusOAuthManager::DeviceFlowSlot::operator=(
    DeviceFlowSlot&&) = default;
NexusOAuthManager::DeviceFlowSlot::~DeviceFlowSlot() = default;

NexusOAuthManager::NexusOAuthManager(
    PrefService* prefs,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    base::RepeatingCallback<void(const std::string& provider_id)>
        on_tokens_saved)
    : prefs_(prefs),
      url_loader_factory_(std::move(url_loader_factory)),
      on_tokens_saved_(std::move(on_tokens_saved)),
      store_(prefs) {}

NexusOAuthManager::~NexusOAuthManager() = default;

std::string NexusOAuthManager::BeginLogin(const std::string& provider_id) {
  const OAuthProviderConfig* config = GetOAuthProviderConfig(provider_id);
  if (!config) {
    DLOG(ERROR) << "BeginLogin for non-OAuth provider: " << provider_id;
    return std::string();
  }

  CleanExpiredFlows();

  const std::string code_verifier = GenerateRandomBase64Url(32);
  const std::string code_challenge = ComputeS256Challenge(code_verifier);
  const std::string state = GenerateRandomBase64Url(16);

  PendingFlow flow;
  flow.provider_id = provider_id;
  flow.code_verifier = code_verifier;
  flow.created_at = base::Time::Now();
  pending_flows_[state] = std::move(flow);

  std::string scope;
  for (const std::string& s : config->scopes) {
    if (!scope.empty()) {
      scope += ' ';
    }
    scope += s;
  }

  std::string url = config->auth_endpoint;
  url += "?response_type=code";
  url += "&client_id=" + FormEscape(config->client_id);
  url += "&redirect_uri=" + FormEscape(kNexusOAuthRedirectUri);
  url += "&code_challenge=" + FormEscape(code_challenge);
  url += "&code_challenge_method=S256";
  url += "&scope=" + FormEscape(scope);
  url += "&state=" + FormEscape(state);
  for (const auto& [key, value] : config->extra_auth_params) {
    url += "&" + FormEscape(key) + "=" + FormEscape(value);
  }
  return url;
}

void NexusOAuthManager::HandleAuthCode(const std::string& code,
                                       const std::string& state,
                                       AuthCodeCallback callback) {
  auto it = pending_flows_.find(state);
  if (it == pending_flows_.end()) {
    std::move(callback).Run(false, "Invalid or expired OAuth state");
    return;
  }
  const PendingFlow flow = it->second;
  pending_flows_.erase(it);

  if (base::Time::Now() - flow.created_at > kFlowTtl) {
    std::move(callback).Run(false, "OAuth flow expired. Please try again.");
    return;
  }

  const OAuthProviderConfig* config = GetOAuthProviderConfig(flow.provider_id);
  if (!config) {
    std::move(callback).Run(false, "Unknown OAuth provider");
    return;
  }

  std::string body = "grant_type=authorization_code";
  body += "&client_id=" + FormEscape(config->client_id);
  body += "&code=" + FormEscape(code);
  body += "&redirect_uri=" + FormEscape(kNexusOAuthRedirectUri);
  body += "&code_verifier=" + FormEscape(flow.code_verifier);

  PostTokenRequest(
      config->token_endpoint, body,
      base::BindOnce(&NexusOAuthManager::OnAuthCodeExchanged,
                     weak_factory_.GetWeakPtr(), flow.provider_id,
                     std::move(callback)));
}

void NexusOAuthManager::StartDeviceCodeFlow(const std::string& provider_id,
                                            DeviceCodeCallback callback,
                                            DeviceCodeDoneCallback done) {
  const OAuthProviderConfig* config = GetOAuthProviderConfig(provider_id);
  if (!config || config->flow != OAuthFlow::kDeviceCode) {
    std::move(callback).Run(false, DeviceCodeInfo(),
                            "Unknown device-code provider");
    return;
  }
  DeviceFlowSlot& slot = device_flows_[provider_id];
  slot.generation = next_device_flow_generation_++;
  slot.done = std::move(done);

  std::string scope;
  for (const std::string& s : config->scopes) {
    if (!scope.empty()) {
      scope += ' ';
    }
    scope += s;
  }

  std::vector<std::pair<std::string, std::string>> params = {
      {"client_id", config->client_id},
      {"scope", scope},
  };
  std::string code_verifier;
  if (config->device_code_requires_pkce) {
    code_verifier = GenerateRandomBase64Url(32);
    params.emplace_back("code_challenge", ComputeS256Challenge(code_verifier));
    params.emplace_back("code_challenge_method", "S256");
  }

  PostForJson(
      config->auth_endpoint, config->device_code_use_form,
      BuildBody(params, config->device_code_use_form),
      base::BindOnce(&NexusOAuthManager::OnDeviceCodeStarted,
                     weak_factory_.GetWeakPtr(), provider_id, code_verifier,
                     slot.generation, std::move(callback)));
}

void NexusOAuthManager::CancelDeviceCodeFlow(const std::string& provider_id) {
  device_flows_.erase(provider_id);
}

bool NexusOAuthManager::IsCurrentDeviceFlow(
    const DeviceFlowState& state) const {
  auto it = device_flows_.find(state.provider_id);
  return it != device_flows_.end() &&
         it->second.generation == state.generation;
}

void NexusOAuthManager::FinishDeviceFlow(const DeviceFlowState& state,
                                         bool ok,
                                         std::string error) {
  auto it = device_flows_.find(state.provider_id);
  if (it == device_flows_.end() || it->second.generation != state.generation) {
    return;
  }
  DeviceCodeDoneCallback done = std::move(it->second.done);
  device_flows_.erase(it);
  if (done) {
    std::move(done).Run(ok, std::move(error));
  }
}

void NexusOAuthManager::OnDeviceCodeStarted(
    const std::string& provider_id,
    const std::string& code_verifier,
    uint64_t generation,
    DeviceCodeCallback callback,
    std::optional<base::DictValue> response) {
  DeviceFlowState state;
  state.provider_id = provider_id;
  state.generation = generation;
  if (!IsCurrentDeviceFlow(state)) {
    std::move(callback).Run(false, DeviceCodeInfo(),
                            "Sign-in was cancelled");
    return;
  }
  // A failed start reports through `callback` only; the slot goes with it.
  auto fail = [&](std::string error) {
    device_flows_.erase(provider_id);
    std::move(callback).Run(false, DeviceCodeInfo(), std::move(error));
  };
  if (!response) {
    fail("Failed to start device authorization");
    return;
  }

  const std::string* device_code = response->FindString("device_code");
  const std::string* user_code = response->FindString("user_code");
  if (!device_code || !user_code) {
    const std::string* error = response->FindString("error");
    fail(error ? *error : "Invalid device authorization response");
    return;
  }

  DeviceCodeInfo info;
  info.user_code = *user_code;
  if (const std::string* complete =
          response->FindString("verification_uri_complete")) {
    info.verification_uri = *complete;
  } else if (const std::string* uri =
                 response->FindString("verification_uri")) {
    info.verification_uri = *uri;
  }
  info.expires_in = response->FindInt("expires_in").value_or(0);
  std::move(callback).Run(true, info, std::string());

  // Begin background polling for the token.
  state.device_code = *device_code;
  state.code_verifier = code_verifier;
  const int interval = response->FindInt("interval").value_or(5);
  state.interval = base::Seconds(interval > 0 ? interval : 5);
  const int expires = info.expires_in > 0 ? info.expires_in : 900;
  state.deadline = base::TimeTicks::Now() + base::Seconds(expires);

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&NexusOAuthManager::PollDeviceCode,
                     weak_factory_.GetWeakPtr(), state),
      state.interval);
}

void NexusOAuthManager::PollDeviceCode(DeviceFlowState state) {
  if (!IsCurrentDeviceFlow(state)) {
    return;
  }
  if (base::TimeTicks::Now() > state.deadline) {
    FinishDeviceFlow(state, false, std::string());
    return;
  }
  const OAuthProviderConfig* config = GetOAuthProviderConfig(state.provider_id);
  if (!config) {
    FinishDeviceFlow(state, false, std::string());
    return;
  }

  std::vector<std::pair<std::string, std::string>> params = {
      {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
      {"client_id", config->client_id},
      {"device_code", state.device_code},
  };
  if (!state.code_verifier.empty()) {
    params.emplace_back("code_verifier", state.code_verifier);
  }

  PostForJson(config->token_endpoint, config->device_code_use_form,
              BuildBody(params, config->device_code_use_form),
              base::BindOnce(&NexusOAuthManager::OnDeviceCodePoll,
                             weak_factory_.GetWeakPtr(), state));
}

void NexusOAuthManager::OnDeviceCodePoll(
    DeviceFlowState state,
    std::optional<base::DictValue> response) {
  if (!IsCurrentDeviceFlow(state)) {
    return;
  }
  auto reschedule = [&]() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NexusOAuthManager::PollDeviceCode,
                       weak_factory_.GetWeakPtr(), state),
        state.interval);
  };

  if (!response) {
    reschedule();  // Transient network error — keep polling until the deadline.
    return;
  }

  if (const std::string* access = response->FindString("access_token");
      access && !access->empty()) {
    OAuthTokens tokens;
    tokens.access_token = *access;
    if (const std::string* refresh = response->FindString("refresh_token")) {
      tokens.refresh_token = *refresh;
    }
    const int expires_in = response->FindInt("expires_in").value_or(0);
    tokens.expires_at_ms =
        expires_in > 0
            ? (base::Time::Now() + base::Seconds(expires_in))
                  .InMillisecondsFSinceUnixEpoch()
            : 0;
    // Per-account API host (Qwen Code) — SaveTokens normalizes it to base_url.
    if (const std::string* resource_url =
            response->FindString("resource_url")) {
      tokens.resource_url = *resource_url;
    }
    // Best-effort claim extraction (no-op for opaque GitHub tokens).
    ExtractClaims(*access, &tokens.account_id, &tokens.email);

    store_.SaveTokens(state.provider_id, tokens);
    if (on_tokens_saved_) {
      on_tokens_saved_.Run(state.provider_id);
    }
    FinishDeviceFlow(state, true, std::string());
    return;
  }

  const std::string* error = response->FindString("error");
  if (error && *error == "authorization_pending") {
    reschedule();
    return;
  }
  if (error && *error == "slow_down") {
    state.interval += base::Seconds(5);
    reschedule();
    return;
  }
  // expired_token / access_denied / anything else — stop polling.
  FinishDeviceFlow(state, false, error ? *error : std::string());
}

void NexusOAuthManager::PostForJson(const std::string& url,
                                    bool use_form,
                                    const std::string& body,
                                    JsonResponseCallback callback) {
  const char* content_type =
      use_form ? "application/x-www-form-urlencoded" : "application/json";

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url);
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("Content-Type", content_type);
  request->headers.SetHeader("Accept", "application/json");

  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 kTrafficAnnotation);
  loader->AttachStringForUpload(body, content_type);
  loader->SetAllowHttpErrorResults(true);

  const std::string loader_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto* loader_ptr = loader.get();
  loaders_.emplace(loader_id, std::move(loader));

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&NexusOAuthManager::OnPostForJson,
                     weak_factory_.GetWeakPtr(), loader_id, std::move(callback)),
      kMaxResponseSizeBytes);
}

void NexusOAuthManager::OnPostForJson(
    const std::string& loader_id,
    JsonResponseCallback callback,
    std::optional<std::string> response_body) {
  loaders_.erase(loader_id);
  if (!response_body) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::move(callback).Run(
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC));
}

void NexusOAuthManager::GetForJson(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    JsonResponseCallback callback) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url);
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  for (const auto& [name, value] : headers) {
    request->headers.SetHeader(name, value);
  }

  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 kTrafficAnnotation);
  loader->SetAllowHttpErrorResults(true);

  const std::string loader_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto* loader_ptr = loader.get();
  loaders_.emplace(loader_id, std::move(loader));

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&NexusOAuthManager::OnPostForJson,
                     weak_factory_.GetWeakPtr(), loader_id, std::move(callback)),
      kMaxResponseSizeBytes);
}

void NexusOAuthManager::DeliverBackendToken(const std::string& provider_id,
                                            const std::string& durable_token,
                                            const std::string& account_id,
                                            EnsureTokenCallback callback) {
  // GitHub Copilot's durable token is a GitHub OAuth token; the backend wants
  // the short-lived Copilot session token minted from it. Every other provider
  // uses its stored access token directly.
  if (provider_id == "github-copilot") {
    EnsureCopilotToken(durable_token, std::move(callback));
    return;
  }
  std::move(callback).Run(true, durable_token, account_id);
}

void NexusOAuthManager::EnsureCopilotToken(const std::string& github_token,
                                           EnsureTokenCallback callback) {
  if (!copilot_access_token_.empty() &&
      copilot_token_source_ == github_token &&
      base::Time::Now() < copilot_token_expiry_ - kExpiryBuffer) {
    std::move(callback).Run(true, copilot_access_token_, std::string());
    return;
  }

  // Exchange the GitHub OAuth token for a Copilot session token. The editor
  // headers identify a Copilot-entitled client; without them the endpoint
  // returns 403 even with a valid GitHub token.
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Authorization", "token " + github_token},
      {"Accept", "application/json"},
      {"Editor-Version", "vscode/1.96.0"},
      {"Editor-Plugin-Version", "copilot-chat/0.23.0"},
      {"User-Agent", "GitHubCopilotChat/0.23.0"},
  };
  GetForJson("https://api.github.com/copilot_internal/v2/token",
             std::move(headers),
             base::BindOnce(&NexusOAuthManager::OnCopilotTokenFetched,
                            weak_factory_.GetWeakPtr(), github_token,
                            std::move(callback)));
}

void NexusOAuthManager::OnCopilotTokenFetched(
    const std::string& github_token,
    EnsureTokenCallback callback,
    std::optional<base::DictValue> response) {
  if (!response) {
    std::move(callback).Run(false, std::string(), std::string());
    return;
  }
  const std::string* token = response->FindString("token");
  if (!token || token->empty()) {
    std::move(callback).Run(false, std::string(), std::string());
    return;
  }

  copilot_access_token_ = *token;
  copilot_token_source_ = github_token;
  // `expires_at` is a unix timestamp in seconds. Fall back to a conservative
  // lifetime if the field is missing so we still refresh periodically.
  if (std::optional<double> expires_at = response->FindDouble("expires_at")) {
    copilot_token_expiry_ = base::Time::FromMillisecondsSinceUnixEpoch(
        static_cast<int64_t>(*expires_at * 1000));
  } else {
    copilot_token_expiry_ = base::Time::Now() + base::Minutes(25);
  }
  std::move(callback).Run(true, copilot_access_token_, std::string());
}

void NexusOAuthManager::EnsureFreshToken(const std::string& provider_id,
                                         EnsureTokenCallback callback) {
  std::optional<OAuthTokens> tokens = store_.GetTokens(provider_id);
  if (!tokens) {
    std::move(callback).Run(false, std::string(), std::string());
    return;
  }

  const bool fresh =
      tokens->expires_at_ms == 0 ||
      base::Time::Now() < base::Time::FromMillisecondsSinceUnixEpoch(
                              static_cast<int64_t>(tokens->expires_at_ms)) -
                              kExpiryBuffer;
  if (fresh) {
    DeliverBackendToken(provider_id, tokens->access_token, tokens->account_id,
                        std::move(callback));
    return;
  }

  if (tokens->refresh_token.empty()) {
    std::move(callback).Run(false, std::string(), std::string());
    return;
  }

  const bool refresh_in_flight = refresh_waiters_.count(provider_id) > 0;
  refresh_waiters_[provider_id].push_back(std::move(callback));
  if (refresh_in_flight) {
    return;
  }

  const OAuthProviderConfig* config = GetOAuthProviderConfig(provider_id);
  if (!config) {
    RunRefreshWaiters(provider_id, false, std::string(), std::string());
    return;
  }

  std::string body = "grant_type=refresh_token";
  body += "&client_id=" + FormEscape(config->client_id);
  body += "&refresh_token=" + FormEscape(tokens->refresh_token);

  PostTokenRequest(config->token_endpoint, body,
                   base::BindOnce(&NexusOAuthManager::OnRefreshComplete,
                                  weak_factory_.GetWeakPtr(), provider_id));
}

bool NexusOAuthManager::IsConnected(const std::string& provider_id) const {
  return store_.HasTokens(provider_id);
}

std::string NexusOAuthManager::GetConnectedEmail(
    const std::string& provider_id) const {
  std::optional<OAuthTokens> tokens = store_.GetTokens(provider_id);
  return tokens ? tokens->email : std::string();
}

void NexusOAuthManager::PostTokenRequest(const std::string& token_endpoint,
                                         const std::string& form_body,
                                         TokenRequestCallback callback) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(token_endpoint);
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("Content-Type",
                             "application/x-www-form-urlencoded");
  request->headers.SetHeader("Accept", "application/json");

  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 kTrafficAnnotation);
  loader->AttachStringForUpload(form_body, "application/x-www-form-urlencoded");
  loader->SetAllowHttpErrorResults(true);

  const std::string loader_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto* loader_ptr = loader.get();
  loaders_.emplace(loader_id, std::move(loader));

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&NexusOAuthManager::OnTokenResponse,
                     weak_factory_.GetWeakPtr(), loader_id,
                     std::move(callback)),
      kMaxResponseSizeBytes);
}

void NexusOAuthManager::OnTokenResponse(
    const std::string& loader_id,
    TokenRequestCallback callback,
    std::optional<std::string> response_body) {
  loaders_.erase(loader_id);

  if (!response_body) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!dict) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  if (dict->Find("error")) {
    DLOG(ERROR) << "OAuth token endpoint returned error";
    std::move(callback).Run(std::nullopt);
    return;
  }
  const std::string* access = dict->FindString("access_token");
  if (!access || access->empty()) {
    std::move(callback).Run(std::nullopt);
    return;
  }

  TokenResponse response;
  response.access_token = *access;
  if (const std::string* refresh = dict->FindString("refresh_token")) {
    response.refresh_token = *refresh;
  }
  response.expires_in = dict->FindInt("expires_in").value_or(0);
  if (const std::string* id_token = dict->FindString("id_token")) {
    response.id_token = *id_token;
  }
  if (const std::string* resource_url = dict->FindString("resource_url")) {
    response.resource_url = *resource_url;
  }
  std::move(callback).Run(std::move(response));
}

void NexusOAuthManager::OnAuthCodeExchanged(
    const std::string& provider_id,
    AuthCodeCallback callback,
    std::optional<TokenResponse> response) {
  if (!response) {
    std::move(callback).Run(false, "Token exchange failed");
    return;
  }

  OAuthTokens tokens;
  tokens.access_token = response->access_token;
  tokens.refresh_token = response->refresh_token;
  tokens.expires_at_ms =
      response->expires_in > 0
          ? (base::Time::Now() + base::Seconds(response->expires_in))
                .InMillisecondsFSinceUnixEpoch()
          : 0;
  ExtractClaims(
      !response->id_token.empty() ? response->id_token : response->access_token,
      &tokens.account_id, &tokens.email);

  store_.SaveTokens(provider_id, tokens);
  if (on_tokens_saved_) {
    on_tokens_saved_.Run(provider_id);
  }
  std::move(callback).Run(true, std::string());
}

void NexusOAuthManager::OnRefreshComplete(
    const std::string& provider_id,
    std::optional<TokenResponse> response) {
  if (!response) {
    RunRefreshWaiters(provider_id, false, std::string(), std::string());
    return;
  }

  std::optional<OAuthTokens> existing = store_.GetTokens(provider_id);

  OAuthTokens tokens;
  tokens.access_token = response->access_token;
  tokens.refresh_token =
      !response->refresh_token.empty()
          ? response->refresh_token
          : (existing ? existing->refresh_token : std::string());
  tokens.expires_at_ms =
      response->expires_in > 0
          ? (base::Time::Now() + base::Seconds(response->expires_in))
                .InMillisecondsFSinceUnixEpoch()
          : 0;
  std::string account_id;
  std::string email;
  ExtractClaims(
      !response->id_token.empty() ? response->id_token : response->access_token,
      &account_id, &email);
  tokens.account_id =
      !account_id.empty() ? account_id
                          : (existing ? existing->account_id : std::string());
  tokens.email =
      !email.empty() ? email : (existing ? existing->email : std::string());
  // Carry forward the per-account API host (Qwen Code) across refresh — the
  // refresh response usually omits it.
  tokens.resource_url =
      !response->resource_url.empty()
          ? response->resource_url
          : (existing ? existing->resource_url : std::string());

  store_.SaveTokens(provider_id, tokens);
  if (on_tokens_saved_) {
    on_tokens_saved_.Run(provider_id);
  }
  // github-copilot's stored token is a GitHub OAuth token, not a Copilot
  // bearer; mint the Copilot session token before handing it to waiters.
  if (provider_id == "github-copilot") {
    EnsureCopilotToken(
        tokens.access_token,
        base::BindOnce(
            [](base::WeakPtr<NexusOAuthManager> self, std::string pid, bool ok,
               std::string token, std::string account) {
              if (self) {
                self->RunRefreshWaiters(pid, ok, token, account);
              }
            },
            weak_factory_.GetWeakPtr(), provider_id));
    return;
  }
  RunRefreshWaiters(provider_id, true, tokens.access_token, tokens.account_id);
}

void NexusOAuthManager::RunRefreshWaiters(const std::string& provider_id,
                                          bool ok,
                                          const std::string& access_token,
                                          const std::string& account_id) {
  auto it = refresh_waiters_.find(provider_id);
  if (it == refresh_waiters_.end()) {
    return;
  }
  std::vector<EnsureTokenCallback> waiters = std::move(it->second);
  refresh_waiters_.erase(it);
  for (auto& waiter : waiters) {
    std::move(waiter).Run(ok, access_token, account_id);
  }
}

void NexusOAuthManager::CleanExpiredFlows() {
  const base::Time now = base::Time::Now();
  for (auto it = pending_flows_.begin(); it != pending_flows_.end();) {
    if (now - it->second.created_at > kFlowTtl) {
      it = pending_flows_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace nexus
