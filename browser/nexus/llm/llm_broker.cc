#include "custom_browser/browser/nexus/llm/llm_broker.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/uuid.h"
#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/llm/llm_pref_registration.h"
#include "custom_browser/browser/nexus/llm/llm_provider.h"
#include "custom_browser/browser/nexus/llm/llm_provider_factory.h"
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
    : prefs_(prefs), url_loader_factory_(std::move(url_loader_factory)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  InitializeProviders();
  LoadProviderConfigs();
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

  if (!SetActiveProvider(provider_id)) {
    result.error_code = "INTERNAL";
    result.error_message = "Failed to switch active provider";
    return result;
  }

  result.ok = true;
  result.request_id = base::Uuid::GenerateRandomV4().AsLowercaseString();

  // Push the rebind event into nexus_web. nexus_web's React shell
  // calls `setModelSelection` on the active session client, which posts
  // `worker.setModelSelection` to the SharedWorker, which updates the
  // engine and re-pushes a fresh SessionStatus. That status broadcast
  // is what the WebUI quick-settings popup listens for. We deliberately
  // do NOT update `session_status_` optimistically here — doing so
  // makes the popup trigger label flip before the worker has actually
  // applied the pick, which lies to anyone (test or human) who reads
  // it as "the next request will use this model". The global-default
  // pref write above is the only durable guarantee.
  // Fan out to every registered pusher (one per live bridge port).
  // The page surface where MultiSessionShell subscribes might be on a
  // different port than the worker surface that fielded the original
  // chrome.send, so a single-pusher slot would race-and-lose.
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
  infos.reserve(registered_ids.size());

  for (const std::string& id : registered_ids) {
    auto it = providers_.find(id);
    if (it == providers_.end()) {
      continue;
    }
    const LlmProvider& provider = *it->second;

    ProviderInfo info;
    info.id = provider.GetId();
    info.name = provider.GetDisplayName();
    info.type = provider.GetProviderType();
    info.is_configured = provider.IsConfigured();
    info.models = provider.GetSupportedModels();
    infos.push_back(std::move(info));
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

  provider->FetchAvailableModels(
      base::BindOnce(&LlmBroker::OnCatalogRefreshed,
                     weak_factory_.GetWeakPtr(), provider_id));
}

void LlmBroker::ReloadProvidersFromPrefs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  providers_.clear();
  active_provider_id_.clear();
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

    LlmProvider* provider = GetProvider(provider_id);
    if (provider) {
      provider->Configure(config_val.GetDict());
    }
  }
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
}

}  // namespace nexus
