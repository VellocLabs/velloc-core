#include "custom_browser/browser/nexus/api/llm/llm_broker.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/uuid.h"
#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_registration.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace nexus {

LlmBroker::DispatchPickResult::DispatchPickResult() = default;
LlmBroker::DispatchPickResult::~DispatchPickResult() = default;
LlmBroker::DispatchPickResult::DispatchPickResult(const DispatchPickResult&) =
    default;
LlmBroker::DispatchPickResult& LlmBroker::DispatchPickResult::operator=(
    const DispatchPickResult&) = default;
LlmBroker::DispatchPickResult::DispatchPickResult(DispatchPickResult&&) =
    default;
LlmBroker::DispatchPickResult& LlmBroker::DispatchPickResult::operator=(
    DispatchPickResult&&) = default;

LlmBroker::LlmBroker(
    PrefService* prefs,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : prefs_(prefs), url_loader_factory_(url_loader_factory) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  oauth_manager_ = std::make_unique<NexusOAuthManager>(
      prefs_, url_loader_factory_,
      base::BindRepeating(&LlmBroker::OnOAuthTokensSaved,
                          base::Unretained(this)));
  InitializeProviders();
  LoadProviderConfigs();
  // At-rest secret encryption: api_key + OAuth tokens are stored encrypted
  // and decrypted in LlmProvider::Configure() / OAuthTokenStore. The OS
  // encryptor is primed by higher-level code (NexusBridgePortService /
  // settings handler — they can reach g_browser_process without a
  // dependency cycle). If it isn't ready yet at this cold-start read,
  // re-apply configs once it arrives so a decrypt-to-empty doesn't leave
  // providers unconfigured. (Writes + later reconfigures happen long after
  // startup, when it's already cached.)
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  if (!crypt->ready()) {
    crypt->WhenReady(base::BindOnce(&LlmBroker::LoadProviderConfigs,
                                    weak_factory_.GetWeakPtr()));
  }
  // Warm-load the TS-pushed caches from prefs so the WebUI quick-settings
  // popup can render *something* on cold boot before nexus_web has had a
  // chance to push fresh data via `model.publishCatalog` /
  // `session.publishStatus`.
  ts_model_catalog_ = prefs_->GetDict(prefs::kNexusTsModelCatalogCache).Clone();
  session_status_ = prefs_->GetDict(prefs::kNexusLastSessionStatus).Clone();
  // Model catalogues are TS-authoritative (see
  // `nexus_web/src/browser/config/nexusModelConfig.ts`). No network discovery
  // is performed — the hard-coded config supplies every id, context window,
  // and max-output-tokens value the UI renders.
}

void LlmBroker::OnOAuthTokensSaved(const std::string& provider_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The OAuth manager just persisted tokens for `provider_id` (login or
  // refresh). Reconfigure the in-memory provider from prefs — the access
  // token is mirrored into `api_key` — so IsConfigured() flips true right
  // away. Without this the provider's catalog entry stays disabled (see
  // BuildCatalog's `provider_configured` gate) until the next browser start.
  const base::DictValue& all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs);
  const base::DictValue* config = all_configs.FindDict(provider_id);
  if (!config) {
    return;
  }
  if (LlmProvider* provider = GetProvider(provider_id)) {
    provider->Configure(*config);
    // Discover the account's models now (the provider is configured), so the
    // picker is populated promptly after sign-in rather than waiting for the
    // next catalog refresh. No-op for providers without /models discovery.
    RefreshCatalogForProvider(provider_id);
  }
}

LlmBroker::~LlmBroker() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

// static
void LlmBroker::RegisterProfilePrefs(PrefRegistrySimple* registry) {
  RegisterNexusProfilePrefs(registry);
}

void LlmBroker::SetTsModelCatalog(base::DictValue catalog) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ts_model_catalog_ = std::move(catalog);
  // Persist a clone so cold-boot has the latest snapshot ready before the
  // guest reconnects. Cheap (catalog is small JSON) and keeps the WebUI
  // popup correct across browser restarts.
  prefs_->SetDict(prefs::kNexusTsModelCatalogCache, ts_model_catalog_.Clone());

  for (auto& observer : observers_) {
    observer.OnTsModelCatalogChanged(ts_model_catalog_);
  }
}

namespace {
// Translate the settings.json camelCase provider-config keys to the legacy
// snake_case shape `LlmProvider::Configure` + the `nexus.provider_configs`
// pref expect, overlaying onto `out` (so existing pref-only fields — e.g.
// OAuth tokens — survive when the settings.json config doesn't carry them).
// Only the user-editable fields that affect Configure are translated;
// transient OAuth scratch + verification stamps stay settings-side.
void ApplySettingsConfigToPrefDict(const base::DictValue& settings,
                                   base::DictValue& out) {
  // Capture the pref's CURRENT api_key BEFORE copy_string overwrites it —
  // the verification-stamp logic below needs to know whether the incoming
  // settings config is changing the key (see the last_verification block).
  const std::string* existing_api_key = out.FindString("api_key");
  const std::string existing_api_key_str =
      existing_api_key ? *existing_api_key : std::string();
  auto copy_string = [&](const char* from, const char* to) {
    if (const std::string* v = settings.FindString(from)) {
      out.Set(to, *v);
    }
  };
  copy_string("apiKey", "api_key");
  copy_string("baseUrl", "base_url");
  copy_string("defaultModel", "default_model");
  copy_string("model", "model");
  // Stream-guard overrides (ints). settings.json is the source of truth:
  // mirror presence — an absent key removes a stale override so the provider
  // falls back to its compiled-in default.
  auto copy_int = [&](const char* from, const char* to) {
    if (std::optional<int> v = settings.FindInt(from)) {
      out.Set(to, *v);
    } else {
      out.Remove(to);
    }
  };
  copy_int("streamIdleTimeoutSeconds", "stream_idle_timeout_s");
  copy_int("maxConcurrentStreams", "max_concurrent_streams");
  // User-created custom providers: the identity bits the broker needs to
  // REBUILD the provider on a cold boot, when only the pref survives (see
  // LoadProviderConfigs → EnsureCustomProvider). Without carrying these
  // through, a custom provider would work for the session that created it and
  // then vanish on the next browser start.
  copy_string("displayName", "display_name");
  copy_string("apiCompatibility", "api_compatibility");
  if (std::optional<bool> is_custom = settings.FindBool("isCustomProvider")) {
    out.Set("is_custom_provider", *is_custom);
  }
  if (const base::ListValue* models = settings.FindList("enabledModels")) {
    out.Set("enabled_models", models->Clone());
  }
  // Carry the verification stamp through to the pref so BuildCatalog's
  // `provider_verified` gate (IsProviderVerified → last_verification.
  // verifiedAtMs) flips the provider's catalog entry enabled after a
  // settings-side (React `providers.upsert` → `publishProviderConfigs`) save —
  // the path that replaced the legacy `updateProviderConfig` →
  // RefreshCatalogForProvider flow. Without this the quick model picker stays
  // empty after configuring a provider even though the key is saved and works
  // for LLM calls. The inner keys stay camelCase (providerId / verifiedAtMs):
  // IsProviderVerified reads them as-is; only the outer key is snake_case to
  // match the legacy pref shape.
  //
  // Drop a stale stamp ONLY when the api_key actually changed — mirroring
  // mergeProviderConfigForSave (providerConfigMerge.ts), which clears
  // lastVerification when `form.apiKey !== saved.apiKey`. The previous
  // unconditional `Remove` wiped the pref's existing stamp whenever the
  // incoming settings config omitted `lastVerification` — including the
  // boot-time `publishProviderConfigsAtBoot` push, which carries only
  // stream-guard overrides for providers whose apiKey lives in the pref
  // (the e2e harness seeds apiKey + last_verification into the pref, NOT
  // settings.json). That left `has_key=1 verified=0` and disabled every
  // configured provider in the catalog. When the key is unchanged (or the
  // settings config carries no key at all), keep the existing stamp.
  if (const base::DictValue* last_verification =
          settings.FindDict("lastVerification")) {
    out.Set("last_verification", last_verification->Clone());
  } else if (const std::string* new_api_key = settings.FindString("apiKey");
             new_api_key && *new_api_key != existing_api_key_str) {
    out.Remove("last_verification");
  }
}
}  // namespace

void LlmBroker::SetProviderConfigs(base::DictValue configs,
                                   const std::string& active_provider_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // settings.json is the source of truth for the user-editable fields; merge
  // them onto the existing pref per provider so C++-owned secrets (OAuth
  // tokens) survive, configure the live provider, then write the pref back as
  // the cold-boot cache `LoadProviderConfigs` reads.
  base::DictValue all = prefs_->GetDict(prefs::kNexusProviderConfigs).Clone();
  for (const auto [provider_id, config_val] : configs) {
    if (!config_val.is_dict()) {
      continue;
    }
    base::DictValue merged;
    if (const base::DictValue* existing = all.FindDict(provider_id)) {
      merged = existing->Clone();
    }
    ApplySettingsConfigToPrefDict(config_val.GetDict(), merged);
    // A user-created custom provider has no compiled-in id, so it must be
    // materialised before it can be configured — otherwise every request
    // addressed to it dies at `GetProvider() == nullptr`.
    EnsureCustomProvider(provider_id, merged);
    if (LlmProvider* provider = GetProvider(provider_id)) {
      provider->Configure(merged);
    }
    all.Set(provider_id, std::move(merged));
  }
  prefs_->SetDict(prefs::kNexusProviderConfigs, std::move(all));

  // Active provider — only adopt a known one (mirrors LoadProviderConfigs's
  // guard); empty means "leave the current active selection unchanged".
  if (!active_provider_id.empty() &&
      providers_.find(active_provider_id) != providers_.end()) {
    active_provider_id_ = active_provider_id;
    prefs_->SetString(prefs::kNexusActiveProviderId, active_provider_id);
  }
}

const base::DictValue& LlmBroker::GetTsModelCatalog() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return ts_model_catalog_;
}

void LlmBroker::SetSessionStatus(base::DictValue status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  session_status_ = std::move(status);
  prefs_->SetDict(prefs::kNexusLastSessionStatus, session_status_.Clone());

  for (auto& observer : observers_) {
    observer.OnSessionStatusChanged(session_status_);
  }
}

const base::DictValue& LlmBroker::GetSessionStatus() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return session_status_;
}

void LlmBroker::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void LlmBroker::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

LlmBroker::PickEventPusherToken LlmBroker::AddPickEventPusher(
    PickEventPusher pusher) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PickEventPusherToken token = next_pusher_token_++;
  pick_event_pushers_.emplace(token, std::move(pusher));
  return token;
}

void LlmBroker::RemovePickEventPusher(PickEventPusherToken token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pick_event_pushers_.erase(token);
}

LlmBroker::DispatchPickResult LlmBroker::DispatchUserPick(
    const std::string& provider_id,
    const std::string& model_id,
    const std::string& thinking_tier) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DispatchPickResult result;

  LlmProvider* provider = GetProvider(provider_id);
  if (!provider) {
    result.error_code = "UNKNOWN_PROVIDER";
    result.error_message = "Unknown provider: " + provider_id;
    return result;
  }
  if (!provider->IsConfigured()) {
    result.error_code = "PROVIDER_NOT_CONFIGURED";
    result.error_message =
        "Provider " + provider_id +
        " has no API key configured. Open settings to add one.";
    return result;
  }

  // Persist the global default *before* notifying nexus_web. Even if the
  // guest is unreachable, the user's pick is durable across restarts.
  // Mirrors `NexusLlmHost::SetDefaultSelection` semantics: write both
  // `nexus.active_provider_id` and `provider_configs[id].default_model`.
  base::DictValue all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs).Clone();
  base::DictValue* config = all_configs.EnsureDict(provider_id);
  config->Set("default_model", model_id);
  config->Set("model", model_id);
  prefs_->SetDict(prefs::kNexusProviderConfigs, std::move(all_configs));

  // Mirror the active selection into the unified agent-config dict so it rides
  // the generic `session.applyAgentConfig` channel (the same flow the loop
  // limits use). Writing this pref fires the session adapter's
  // PrefChangeRegistrar, which fans the whole dict out to the guest; the shell
  // routes the `model` key to the active session and updates the new-session
  // default. This is the single source of the guest-facing default selection.
  {
    base::DictValue agent_config =
        prefs_->GetDict(prefs::kNexusAgentConfig).Clone();
    base::DictValue model;
    model.Set("providerId", provider_id);
    model.Set("modelId", model_id);
    if (!thinking_tier.empty()) {
      model.Set("thinkingTier", thinking_tier);
    }
    agent_config.Set("model", std::move(model));
    prefs_->SetDict(prefs::kNexusAgentConfig, std::move(agent_config));
  }

  if (!SetActiveProvider(provider_id)) {
    result.error_code = "INTERNAL";
    result.error_message = "Failed to switch active provider";
    return result;
  }

  result.ok = true;
  result.request_id = base::Uuid::GenerateRandomV4().AsLowercaseString();

  // DEPRECATED legacy delivery (kept for the `pusherInvoked` diagnostic that
  // pickNexusSelection still surfaces). The authoritative live delivery is now
  // the `nexus.agent_config.model` write above, which the session adapter's
  // PrefChangeRegistrar fans out as `session.applyAgentConfig`. The guest no
  // longer subscribes to `session.applySelection`, so these pushes are inert.
  // TODO(nexus): remove the PickEventPusher infra + pusherInvoked once the
  //   quick-settings popup's "no guest listening" diagnostic is reworked to
  //   key off the agent-config push (best done with a build to verify).
  for (const auto& [token, pusher] : pick_event_pushers_) {
    pusher.Run(result.request_id, provider_id, model_id, thinking_tier,
               /*also_update_default=*/true);
    result.pusher_invoked = true;
  }
  return result;
}

LlmProvider* LlmBroker::GetActiveProvider() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (active_provider_id_.empty()) {
    return nullptr;
  }
  return GetProvider(active_provider_id_);
}

std::string LlmBroker::GetActiveProviderId() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return active_provider_id_;
}

bool LlmBroker::SetActiveProvider(const std::string& provider_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (provider_id.empty()) {
    active_provider_id_.clear();
    prefs_->SetString(prefs::kNexusActiveProviderId, std::string());
    return true;
  }

  if (providers_.find(provider_id) == providers_.end()) {
    DLOG(ERROR) << "Cannot set active provider — unknown ID: " << provider_id;
    return false;
  }

  active_provider_id_ = provider_id;
  prefs_->SetString(prefs::kNexusActiveProviderId, provider_id);
  return true;
}

std::vector<ProviderInfo> LlmBroker::GetRegisteredProviders() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Iterate factory declaration order rather than `providers_` (a std::map,
  // ordered alphabetically by key). The Settings UI renders its provider
  // popup straight from this list, so the factory array is the single
  // source of truth for display order.
  std::vector<ProviderInfo> infos;
  const auto registered_ids = LlmProviderFactory::GetRegisteredProviderIds();
  infos.reserve(registered_ids.size() + dynamic_provider_ids_.size());

  auto append = [&](const std::string& id) {
    auto it = providers_.find(id);
    if (it == providers_.end()) {
      return;
    }
    const LlmProvider& provider = *it->second;

    ProviderInfo info;
    info.id = provider.GetId();
    info.name = provider.GetDisplayName();
    info.type = provider.GetProviderType();
    info.is_configured = provider.IsConfigured();
    info.models = provider.GetSupportedModels();
    infos.push_back(std::move(info));
  };

  for (const std::string& id : registered_ids) {
    append(id);
  }
  // User-created custom providers last, in creation order. They MUST appear
  // here: this list is what BuildCatalog walks, so leaving them out would hide
  // them from the guest catalog — and the guest reads exactly that to decide
  // whether it can address a custom provider directly or has to re-point a
  // compiled-in slot at it first.
  for (const std::string& id : dynamic_provider_ids_) {
    append(id);
  }

  return infos;
}

LlmProvider* LlmBroker::GetProvider(const std::string& provider_id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = providers_.find(provider_id);
  return it != providers_.end() ? it->second.get() : nullptr;
}

bool LlmBroker::ConfigureProvider(const std::string& provider_id,
                                  const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetProvider(provider_id);
  if (!provider) {
    DLOG(ERROR) << "Cannot configure unknown provider: " << provider_id;
    return false;
  }

  bool ok = provider->Configure(config);

  // Persist the config dict so it survives browser restart.
  base::DictValue all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs).Clone();
  all_configs.Set(provider_id, config.Clone());
  prefs_->SetDict(prefs::kNexusProviderConfigs, std::move(all_configs));

  return ok;
}

void LlmBroker::RefreshModels(const std::string& provider_id,
                              FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetProvider(provider_id);
  if (!provider) {
    std::move(callback).Run({});
    return;
  }

  // OAuth providers need a backend-ready token before discovery. The stored
  // api_key may be expired (chatgpt-pro/qwen) or not directly usable
  // (github-copilot stores a GitHub OAuth token that must be exchanged for a
  // Copilot session token). Refresh/exchange once here so EVERY discovery path
  // — the guest's cold-boot catalog refresh, the settings connect test — hits
  // the backend with a valid bearer instead of silently 401ing. EnsureFreshToken
  // is a no-op cache hit when the token is already fresh.
  if (oauth_manager_ && IsOAuthProvider(provider_id)) {
    oauth_manager_->EnsureFreshToken(
        provider_id,
        base::BindOnce(&LlmBroker::OnEnsureTokenBeforeRefresh,
                       weak_factory_.GetWeakPtr(), provider_id,
                       std::move(callback)));
    return;
  }

  FetchModelsWithPersist(provider_id, std::move(callback));
}

void LlmBroker::OnEnsureTokenBeforeRefresh(const std::string& provider_id,
                                           FetchModelsCallback callback,
                                           bool ok,
                                           std::string access_token,
                                           std::string account_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (LlmProvider* provider = GetProvider(provider_id);
      provider && ok && !access_token.empty()) {
    base::DictValue config;
    config.Set("api_key", access_token);
    config.Set("account_id", account_id);
    provider->Configure(config);
  }
  // Attempt discovery even if the token couldn't be refreshed — the fetch will
  // surface the auth failure through the normal empty/error path rather than
  // silently skipping.
  FetchModelsWithPersist(provider_id, std::move(callback));
}

void LlmBroker::FetchModelsWithPersist(const std::string& provider_id,
                                       FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetProvider(provider_id);
  if (!provider) {
    std::move(callback).Run({});
    return;
  }

  // Wrap the caller's callback so that every successful fetch — regardless of
  // which code path triggered it — persists the raw JSON blob to the prefs
  // cache. Without this wrap there is a race: `NexusLlmHost::Catalog()` and
  // `RefreshCatalogForProvider()` both call into `FetchAvailableModels`, which
  // shares a single `SimpleURLLoader` member on the provider. Whichever fetch
  // starts second cancels the first (its callback never runs), so if the
  // persist-on-completion hook lives only on one caller's branch, the pref
  // stays empty whenever the other caller wins the race. Persisting here —
  // at the single point where every `RefreshModels` path converges — makes
  // that race harmless: whichever callback actually fires persists.
  provider->FetchAvailableModels(base::BindOnce(
      [](base::WeakPtr<LlmBroker> self, const std::string& provider_id,
         FetchModelsCallback user_callback, std::vector<ModelInfo> models) {
        if (self) {
          self->OnCatalogRefreshed(provider_id, models);
        }
        std::move(user_callback).Run(std::move(models));
      },
      weak_factory_.GetWeakPtr(), provider_id, std::move(callback)));
}

void LlmBroker::RefreshCatalogForProvider(const std::string& provider_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetProvider(provider_id);
  if (!provider || !provider->IsConfigured()) {
    return;
  }

  // Delegate to RefreshModels so OAuth providers get a fresh/exchanged token
  // before discovery (RefreshModels persists the raw catalog + notifies
  // observers via the same OnCatalogRefreshed path). Without this, login-time
  // discovery for github-copilot would fetch with the raw GitHub token (401).
  RefreshModels(provider_id, base::DoNothing());
}

void LlmBroker::ReloadProvidersFromPrefs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  providers_.clear();
  active_provider_id_.clear();
  // Dropped alongside `providers_` so a DELETED custom provider actually goes
  // away: the reset path removes its `kNexusProviderConfigs` entry and then
  // calls here, and LoadProviderConfigs only rebuilds what the pref still
  // carries. Keeping these would leave a phantom provider in the catalog.
  dynamic_provider_ids_.clear();
  dynamic_provider_dialects_.clear();
  InitializeProviders();
  LoadProviderConfigs();
}

// ---------------------------------------------------------------------------
// Inference delegation
// ---------------------------------------------------------------------------

void LlmBroker::DoGenerate(const base::DictValue& call_options,
                           GenerateCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetActiveProvider();
  if (!provider) {
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    std::move(callback).Run(
        std::move(result),
        "No LLM provider configured. Please select a provider in Settings.");
    return;
  }

  provider->DoGenerate(call_options, std::move(callback));
}

void LlmBroker::DoStream(const base::DictValue& call_options,
                         StreamChunkCallback chunk_callback,
                         StreamDoneCallback done_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetActiveProvider();
  if (!provider) {
    std::move(done_callback)
        .Run(
            "No LLM provider configured. Please select a provider in "
            "Settings.");
    return;
  }

  provider->DoStream(call_options, std::move(chunk_callback),
                     std::move(done_callback));
}

void LlmBroker::CancelRequest(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Broadcast to all providers since we may not know which one owns the
  // request (e.g. if the active provider changed mid-request).
  for (auto& [id, provider] : providers_) {
    provider->CancelRequest(request_id);
  }
}

bool LlmBroker::IsRequestQueued(const std::string& request_id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Same broadcast rationale as CancelRequest: request ids are unique
  // (host-generated UUIDs), so at most one provider can claim it.
  for (const auto& [id, provider] : providers_) {
    if (provider->IsRequestQueued(request_id)) {
      return true;
    }
  }
  return false;
}

void LlmBroker::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  providers_.clear();
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void LlmBroker::InitializeProviders() {
  for (const auto& id : LlmProviderFactory::GetRegisteredProviderIds()) {
    auto provider = LlmProviderFactory::CreateProvider(id, url_loader_factory_);
    if (provider) {
      providers_.emplace(id, std::move(provider));
    }
  }
}

void LlmBroker::LoadProviderConfigs() {
  // Restore the active provider from prefs.
  active_provider_id_ = prefs_->GetString(prefs::kNexusActiveProviderId);
  if (!active_provider_id_.empty() &&
      providers_.find(active_provider_id_) == providers_.end()) {
    DLOG(WARNING) << "Persisted active provider '" << active_provider_id_
                  << "' not found; clearing.";
    active_provider_id_.clear();
  }

  // Apply persisted per-provider configs (API keys, base URLs, etc.).
  const base::DictValue& all_configs =
      prefs_->GetDict(prefs::kNexusProviderConfigs);
  for (const auto [provider_id, config_val] : all_configs) {
    if (!config_val.is_dict()) {
      continue;
    }

    // Rebuild user-created custom providers before configuring them — on a
    // cold boot this pref replay is the ONLY thing that knows they exist.
    EnsureCustomProvider(provider_id, config_val.GetDict());

    LlmProvider* provider = GetProvider(provider_id);
    if (provider) {
      provider->Configure(config_val.GetDict());
    }
  }
}

bool LlmBroker::EnsureCustomProvider(const std::string& provider_id,
                                     const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Only configs the settings page marked as user-created qualify. Everything
  // else — built-in providers, and the two compiled-in custom endpoints —
  // already has an instance from InitializeProviders().
  if (!config.FindBool("is_custom_provider").value_or(false)) {
    return false;
  }
  const std::string* dialect = config.FindString("api_compatibility");
  if (!dialect || dialect->empty()) {
    // A custom provider without a dialect cannot be built. Leave it absent
    // rather than guessing: the guest falls back to routing through the
    // compiled-in slot for its dialect, which still works.
    DLOG(WARNING) << "Custom provider " << provider_id
                  << " has no api_compatibility; not instantiating.";
    return false;
  }

  auto existing = dynamic_provider_dialects_.find(provider_id);
  const bool already_built = existing != dynamic_provider_dialects_.end();
  if (already_built && existing->second == *dialect) {
    return true;  // Same provider, same wire format — nothing to do.
  }
  if (!already_built && providers_.find(provider_id) != providers_.end()) {
    // The id collides with a compiled-in provider. Never shadow one: the
    // settings page reserves those ids when generating custom ids, so this
    // only happens with a hand-edited settings.json.
    DLOG(ERROR) << "Custom provider id " << provider_id
                << " collides with a built-in provider; ignoring.";
    return false;
  }

  const std::string* display_name = config.FindString("display_name");
  auto provider = LlmProviderFactory::CreateCustomProvider(
      provider_id, display_name ? *display_name : std::string(), *dialect,
      url_loader_factory_);
  if (!provider) {
    return false;
  }

  // A dialect flip replaces the instance — the wire format is bound at
  // construction. The caller re-applies Configure() right after, so the base
  // URL / key survive the swap.
  providers_[provider_id] = std::move(provider);
  dynamic_provider_dialects_[provider_id] = *dialect;
  if (!already_built) {
    dynamic_provider_ids_.push_back(provider_id);
  }
  return true;
}

void LlmBroker::OnCatalogRefreshed(const std::string& provider_id,
                                   std::vector<ModelInfo> models) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LlmProvider* provider = GetProvider(provider_id);
  if (!provider)
    return;

  std::string raw = provider->GetLastFetchedRawCatalogJson();
  if (raw.empty()) {
    // Fetch failed (network error or empty body) — keep whatever we previously
    // cached so the UI still has something to render.
    return;
  }

  base::DictValue all_catalogs =
      prefs_->GetDict(prefs::kNexusProviderCatalogs).Clone();
  all_catalogs.Set(provider_id, std::move(raw));
  prefs_->SetDict(prefs::kNexusProviderCatalogs, std::move(all_catalogs));

  // Notify observers (the WebUI settings handler) so the topbar quick-switch
  // popup rebuilds now that `provider`'s fetched_models_ are populated. This is
  // what surfaces lazily-discovered OAuth models (chatgpt-pro) on cold boot —
  // the guest's catalog refresh triggers the fetch, but only this signal tells
  // the always-mounted popup to pick the results up.
  for (auto& observer : observers_) {
    observer.OnProviderModelsRefreshed(provider_id);
  }
}

}  // namespace nexus
