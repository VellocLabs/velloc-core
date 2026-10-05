#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/json/json_reader.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/grit/custom_browser_resources.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "ui/base/resource/resource_bundle.h"

namespace nexus {

namespace {

constexpr base::TimeDelta kCatalogRefreshInterval = base::Seconds(30);

bool IsModelEnabled(PrefService* prefs,
                    const std::string& provider_id,
                    const std::string& model_id) {
  const base::DictValue* config = GetStoredProviderConfig(prefs, provider_id);
  if (!config) {
    return true;
  }
  const base::ListValue* enabled_models = config->FindList("enabled_models");
  if (!enabled_models || enabled_models->empty()) {
    return true;
  }
  for (const auto& value : *enabled_models) {
    const std::string* enabled_model = value.GetIfString();
    if (enabled_model && *enabled_model == model_id) {
      return true;
    }
  }
  return false;
}

// Flattens a TS curated-catalog `model` pattern into a literal id. The
// catalog stores each model's `model` field as either a plain id or a
// regex source (anchored, optionally suffixed with a date pattern that
// the OpenAI/Anthropic helpers add). The worker's `isSelectionEnabled`
// gate compares against the literal id the picker sends — so we strip
// the anchors / common date suffixes, then bail if the result still has
// regex metacharacters we don't know how to flatten. (Originally a mirror
// of the retired Lit popup's `deriveModelId`; no guest twin remains.)
std::string DeriveTsModelId(const std::string& pattern) {
  if (pattern.empty()) return std::string();
  std::string s = pattern;
  if (!s.empty() && s.front() == '^') s.erase(s.begin());
  if (!s.empty() && s.back() == '$') s.pop_back();
  // (?:-\d{4}-\d{2}-\d{2})? — OpenAI date suffix.
  constexpr std::string_view kDateY = "(?:-\\d{4}-\\d{2}-\\d{2})?";
  constexpr std::string_view kDate8 = "(?:-\\d{8})?";
  if (s.size() >= kDateY.size() &&
      s.compare(s.size() - kDateY.size(), kDateY.size(), kDateY) == 0) {
    s.resize(s.size() - kDateY.size());
  }
  if (s.size() >= kDate8.size() &&
      s.compare(s.size() - kDate8.size(), kDate8.size(), kDate8) == 0) {
    s.resize(s.size() - kDate8.size());
  }
  // If anything regex-y remains, skip — no literal id can be derived.
  if (s.find_first_of("()[]{}|?+*") != std::string::npos) {
    return std::string();
  }
  // Unescape `\.` etc.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out.push_back(s[i + 1]);
      ++i;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::string ResolveProviderIconSvg(const std::string& provider_id) {
  int resource_id = 0;
  if (provider_id == "openai" || provider_id == "chatgpt-pro") {
    // ChatGPT Plus/Pro reuses the OpenAI brand mark — no separate asset.
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_OPENAI_SVG;
  } else if (provider_id == "anthropic") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_ANTHROPIC_SVG;
  } else if (provider_id == "google") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_GOOGLE_SVG;
  } else if (provider_id == "ollama") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_OLLAMA_SVG;
  } else if (provider_id == "kimi") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_KIMI_SVG;
  } else if (provider_id == "minimax") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_MINIMAX_SVG;
  } else if (provider_id == "qwen" || provider_id == "qwen-code") {
    // Qwen Code (OAuth) reuses the Qwen brand mark.
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_QWEN_SVG;
  } else if (provider_id == "github-copilot") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_GITHUBCOPILOT_SVG;
  } else if (provider_id == "glm") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_GLM_SVG;
  } else if (provider_id == "deepseek") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_DEEPSEEK_SVG;
  } else if (provider_id.starts_with("custom-")) {
    // Every custom endpoint — the two compiled-in dialects and any provider
    // the user created — shares one neutral cube mark; no vendor brand applies
    // to an arbitrary endpoint. (The guest overrides this with the icon the
    // user picked; this is what the browser-side settings UI shows.)
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_CUSTOM_SVG;
  }

  if (resource_id != 0 && ui::ResourceBundle::HasSharedInstance()) {
    std::string icon_svg =
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            resource_id);
    if (!icon_svg.empty()) {
      return icon_svg;
    }
  }
  return R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="24" height="24" rx="12" fill="#475569"/><text x="12" y="15" text-anchor="middle" font-size="11" font-family="Arial, sans-serif" fill="#ffffff">L</text></svg>)";
}

std::string ResolveEnabledModelId(const mojom::NexusLlmProvider& provider) {
  if (provider.default_model_id.has_value() &&
      !provider.default_model_id->empty()) {
    for (const auto& model : provider.models) {
      if (model && model->id == *provider.default_model_id && model->enabled) {
        return model->id;
      }
    }
  }
  for (const auto& model : provider.models) {
    if (model && !model->id.empty() && model->enabled) {
      return model->id;
    }
  }
  return std::string();
}

void MaybeSetDefaultSelection(mojom::NexusLlmCatalog* catalog,
                              const std::string& active_provider_id) {
  if (!catalog) {
    return;
  }

  const mojom::NexusLlmProvider* selected_provider = nullptr;
  if (!active_provider_id.empty()) {
    for (const auto& provider : catalog->providers) {
      if (provider && provider->id == active_provider_id && provider->enabled) {
        selected_provider = provider.get();
        break;
      }
    }
  }
  if (!selected_provider) {
    for (const auto& provider : catalog->providers) {
      if (provider && provider->enabled) {
        selected_provider = provider.get();
        break;
      }
    }
  }
  if (!selected_provider) {
    return;
  }

  const std::string model_id = ResolveEnabledModelId(*selected_provider);
  if (selected_provider->id.empty() || model_id.empty()) {
    return;
  }

  auto selection = mojom::NexusLlmSelection::New();
  selection->provider_id = selected_provider->id;
  selection->model_id = model_id;
  catalog->default_selection = std::move(selection);
}

mojom::NexusLlmCatalogPtr BuildCatalog(Profile* profile) {
  auto catalog = mojom::NexusLlmCatalog::New();

  LlmBroker* broker = profile ? LlmBrokerFactory::GetForProfile(profile) : nullptr;
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!broker) {
    return catalog;
  }

  // Index the TS-pushed catalog by providerId so we can graft its
  // curated models into providers whose own `info.models` came back
  // empty (the common case after we removed `RefreshAllCatalogs` —
  // `GetSupportedModels()` returns nothing). Without this, the worker's
  // `isSelectionEnabled` gate refuses every pick the guest's React model
  // picker (via `providers.setActive` in the SharedWorker) makes, because
  // no model is "enabled" in the bridge catalog the worker consumes.
  const base::DictValue& ts_catalog = broker->GetTsModelCatalog();
  std::map<std::string, const base::DictValue*> ts_by_provider;
  if (const base::ListValue* providers = ts_catalog.FindList("providers")) {
    for (const base::Value& entry : *providers) {
      const base::DictValue* p = entry.GetIfDict();
      if (!p) continue;
      if (const std::string* pid = p->FindString("providerId")) {
        ts_by_provider[*pid] = p;
      }
    }
  }

  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    LlmProvider* provider = broker->GetProvider(info.id);
    // Cold-boot race: the broker's LoadProviderConfigs() runs during
    // LlmBroker construction, which can precede the profile's Preferences
    // file being parsed into the PrefService. When that happens the
    // in-memory provider never gets Configure()d, so IsConfigured() is
    // false here even though a valid api_key sits in prefs — and the
    // catalog lists the provider as disabled. Re-apply the stored config
    // now (prefs is fully loaded by the time BuildCatalog runs) so a
    // late-loading prefs file still yields a configured provider.
    const base::DictValue* stored_config =
        prefs ? GetStoredProviderConfig(prefs, info.id) : nullptr;
    if (provider && !provider->IsConfigured() && stored_config) {
      provider->Configure(*stored_config);
    }
    // For OAuth providers, treat "signed in" (tokens present) as the source of
    // truth for configured — the in-memory api_key can lag behind the stored
    // tokens, which would otherwise hide the provider from the picker even
    // though it works.
    const bool oauth_connected =
        IsOAuthProvider(info.id) && broker->oauth_manager() &&
        broker->oauth_manager()->IsConnected(info.id);
    // SET UP is the whole gate: a saved connection is enough to be pickable,
    // and nothing has to prove it reachable first. This used to be
    // `configured && verified`, where "verified" meant a `providerAuth.verify`
    // probe had once succeeded and stamped `last_verification`
    // (IsProviderVerified). That let a REACHABILITY test decide picker
    // MEMBERSHIP — a provider whose endpoint was down when it was saved, whose
    // credentials were configured outside the settings form, or which sits
    // behind a gateway that serves generation but not `/models`, silently
    // vanished from the quick model picker with nothing in the UI saying why.
    // Settings still runs that probe and still stamps its result (it drives
    // the "Connected" card and `providers.list`'s `isVerified`); it just no
    // longer decides what the user may select. An endpoint that is genuinely
    // unreachable now fails at send time, where the error card can explain it,
    // instead of removing the model from the picker beforehand.
    //
    // The other half of the old gate still has to be replaced, though:
    // `IsConfigured()` means "usable", not "the user set this up", so it is
    // true out of the box for Ollama and for any keyless provider carrying its
    // default base URL. `HasUserProviderSetup` is what keeps those out of a
    // picker on a profile that never configured them.
    const bool provider_configured =
        (provider && provider->IsConfigured()) || oauth_connected;
    const bool provider_set_up =
        provider_configured &&
        (oauth_connected ||
         (stored_config && HasUserProviderSetup(*stored_config)));

    bool has_enabled_model = false;
    std::vector<mojom::NexusLlmModelPtr> models;
    std::set<std::string> seen_ids;
    for (const ModelInfo& model : info.models) {
      const bool model_enabled =
          provider_set_up && IsModelEnabled(prefs, info.id, model.id);
      has_enabled_model = has_enabled_model || model_enabled;

      auto model_ptr = mojom::NexusLlmModel::New();
      model_ptr->id = model.id;
      model_ptr->label = model.name.empty() ? model.id : model.name;
      model_ptr->enabled = model_enabled;
      model_ptr->supports_streaming =
          provider ? provider->SupportsStreaming() : false;
      model_ptr->supports_tools =
          model.supports_tools || (provider ? provider->SupportsTools() : false);
      // Reasoning-effort metadata from dynamic discovery (e.g. the Codex
      // /models endpoint). Lets the guest render the thinking-tier picker
      // without a hardcoded curated entry.
      if (model.default_thinking_tier &&
          !model.default_thinking_tier->empty()) {
        model_ptr->default_thinking_tier = *model.default_thinking_tier;
      }
      model_ptr->available_thinking_tiers = model.available_thinking_tiers;
      seen_ids.insert(model.id);
      models.push_back(std::move(model_ptr));
    }

    // Always splice in TS-curated models — the guest's picker offers
    // them, so the worker's `isSelectionEnabled` gate must accept every
    // curated id. Previously this graft only ran when
    // `info.models` was empty, which silently dropped curated entries
    // for any provider whose `/v1/models` returned a non-empty list
    // that didn't already contain the curated ids (e.g. an OpenAI
    // endpoint that doesn't yet list gpt-5.5, or a Gemini endpoint that
    // names models with a `models/` prefix). DeriveTsModelId flattens
    // each curated pattern to the literal id the worker accepts.
    auto it = ts_by_provider.find(info.id);
    if (it != ts_by_provider.end() && it->second) {
      if (const base::ListValue* ts_models =
              it->second->FindList("models")) {
        for (const base::Value& m_val : *ts_models) {
          const base::DictValue* m = m_val.GetIfDict();
          if (!m) continue;
          const std::string* pattern = m->FindString("model");
          if (!pattern) continue;
          std::string id = DeriveTsModelId(*pattern);
          if (id.empty()) continue;
          // Skip ids already added from `info.models` so we don't
          // duplicate. The worker only inspects id + enabled, so we
          // keep the first entry as-is.
          if (!seen_ids.insert(id).second) continue;

          // Honour the user's per-provider enabled-models allowlist
          // when present; if absent or empty, all curated models are
          // enabled for this configured provider.
          const bool model_enabled =
              provider_set_up && IsModelEnabled(prefs, info.id, id);
          has_enabled_model = has_enabled_model || model_enabled;

          auto model_ptr = mojom::NexusLlmModel::New();
          model_ptr->id = id;
          const std::string* name = m->FindString("name");
          model_ptr->label = (name && !name->empty()) ? *name : id;
          model_ptr->enabled = model_enabled;
          model_ptr->supports_streaming =
              provider ? provider->SupportsStreaming() : false;
          model_ptr->supports_tools =
              provider ? provider->SupportsTools() : false;
          models.push_back(std::move(model_ptr));
        }
      }
    }

    std::string default_model_id =
        stored_config ? GetStoredDefaultModelId(*stored_config) : std::string();
    if (default_model_id.empty() && provider) {
      default_model_id = provider->GetDefaultModelId();
    }

    // Ensure the configured default_model is always selectable. Two cases:
    // 1. Cold boot with no online fetch and no curated graft → the catalog
    //    has zero models; synthesize the default so the provider isn't
    //    empty (the original cold-boot fallback).
    // 2. Cold boot with curated models but no online fetch → the catalog
    //    has curated models (e.g. claude-fable-5) but NOT the user's
    //    configured default_model (e.g. claude-sonnet-4-6 from the fake
    //    server's /v1/models). Without this, setModelSelection to the
    //    configured default fails with worker_invalid_model_selection.
    // Skip if the id is already present (from online fetch or curated
    // graft) to avoid duplicates.
    if (provider_set_up && !default_model_id.empty() &&
        seen_ids.find(default_model_id) == seen_ids.end()) {
      auto model_ptr = mojom::NexusLlmModel::New();
      model_ptr->id = default_model_id;
      model_ptr->label = default_model_id;
      model_ptr->enabled = true;
      model_ptr->supports_streaming =
          provider ? provider->SupportsStreaming() : false;
      model_ptr->supports_tools =
          provider ? provider->SupportsTools() : false;
      has_enabled_model = true;
      models.push_back(std::move(model_ptr));
    }

    auto provider_ptr = mojom::NexusLlmProvider::New();
    provider_ptr->id = info.id;
    provider_ptr->label = info.name;
    provider_ptr->icon_svg = ResolveProviderIconSvg(info.id);
    provider_ptr->enabled = provider_set_up && has_enabled_model;
    if (IsOAuthProvider(info.id)) {
      DLOG(INFO) << "[nexus-catalog] " << info.id
                 << " set_up=" << provider_set_up
                 << " oauth_connected=" << oauth_connected
                 << " fetched_models=" << info.models.size()
                 << " has_enabled_model=" << has_enabled_model
                 << " enabled=" << provider_ptr->enabled;
    }
    if (!default_model_id.empty()) {
      provider_ptr->default_model_id = default_model_id;
    }
    provider_ptr->models = std::move(models);
    catalog->providers.push_back(std::move(provider_ptr));

    if (provider) {
      std::string raw_json = provider->GetLastFetchedRawCatalogJson();
      if (!raw_json.empty()) {
        catalog->raw_catalogs.emplace(info.id, std::move(raw_json));
      }
    }
  }

  MaybeSetDefaultSelection(catalog.get(), broker->GetActiveProviderId());
  return catalog;
}

LlmBroker* GetBroker(Profile* profile) {
  return profile ? LlmBrokerFactory::GetForProfile(profile) : nullptr;
}

bool ProviderHasModelId(const ProviderInfo& provider_info,
                        const std::string& model_id) {
  for (const ModelInfo& model : provider_info.models) {
    if (model.id == model_id) {
      return true;
    }
  }
  return false;
}

}  // namespace

NexusLlmHost::NexusLlmHost(Profile* profile) : profile_(profile) {}

NexusLlmHost::~NexusLlmHost() = default;

void NexusLlmHost::Catalog(CatalogCallback callback) {
  LlmBroker* broker = GetBroker(profile_);
  if (!broker) {
    std::move(callback).Run(true, BuildCatalog(profile_), std::nullopt);
    return;
  }

  // Cold-boot race: the broker's `LoadProviderConfigs` runs at broker
  // construction, which can happen BEFORE the profile's Preferences file
  // is loaded into the PrefService. Re-apply stored configs here so a
  // late-loading prefs file still yields configured providers before we
  // build the `configured_provider_ids` list below.
  if (PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr) {
    for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
      LlmProvider* provider = broker->GetProvider(info.id);
      if (provider && !provider->IsConfigured()) {
        if (const base::DictValue* stored_config =
                GetStoredProviderConfig(prefs, info.id)) {
          provider->Configure(*stored_config);
        }
      }
    }
  }

  std::vector<std::string> configured_provider_ids;
  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    // Check the live provider, not the stale `info.is_configured` snapshot —
    // the re-configure loop above may have just flipped it.
    LlmProvider* provider = broker->GetProvider(info.id);
    if (provider && provider->IsConfigured()) {
      configured_provider_ids.push_back(info.id);
    }
  }

  if (configured_provider_ids.empty()) {
    std::move(callback).Run(true, BuildCatalog(profile_), std::nullopt);
    return;
  }

  const bool can_use_cached_catalog =
      !last_catalog_refresh_time_.is_null() &&
      last_catalog_refresh_provider_ids_ == configured_provider_ids &&
      (base::TimeTicks::Now() - last_catalog_refresh_time_) <
          kCatalogRefreshInterval;
  // On cold boot (first call), skip the model-discovery round-trip and
  // return the prefs-backed catalog immediately. The TS-curated catalog
  // graft in BuildCatalog already supplies every model id the picker needs;
  // discovery only refreshes reasoning-tier metadata. This avoids a
  // BarrierClosure that can stall if any provider's /models endpoint is
  // slow or unreachable on first boot.
  if (can_use_cached_catalog || last_catalog_refresh_time_.is_null()) {
    std::move(callback).Run(true, BuildCatalog(profile_), std::nullopt);
    return;
  }

  base::RepeatingClosure on_provider_models_fetched = base::BarrierClosure(
      configured_provider_ids.size(),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmHost> self, CatalogCallback callback,
             std::vector<std::string> refreshed_provider_ids) {
            if (!self) {
              return;
            }

            self->last_catalog_refresh_time_ = base::TimeTicks::Now();
            self->last_catalog_refresh_provider_ids_ =
                std::move(refreshed_provider_ids);
            std::move(callback).Run(true, BuildCatalog(self->profile_),
                                    std::nullopt);
          },
          weak_factory_.GetWeakPtr(), std::move(callback),
          configured_provider_ids));

  for (const std::string& provider_id : configured_provider_ids) {
    broker->RefreshModels(
        provider_id,
        base::BindOnce(
            [](base::RepeatingClosure done,
               std::vector<ModelInfo> /*models*/) { done.Run(); },
            on_provider_models_fetched));
  }
}

bool NexusLlmHost::SetDefaultSelection(const std::string& provider_id,
                                       const std::string& model_id,
                                       std::string* error_message) {
  if (provider_id.empty() || model_id.empty()) {
    if (error_message) {
      *error_message = "Provider and model ids are required";
    }
    return false;
  }

  LlmBroker* broker = GetBroker(profile_);
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!broker || !prefs) {
    if (error_message) {
      *error_message = "Nexus LLM host is not available";
    }
    return false;
  }

  const ProviderInfo* provider_info = nullptr;
  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    if (info.id == provider_id) {
      provider_info = &info;
      break;
    }
  }
  if (!provider_info) {
    if (error_message) {
      *error_message = "Unknown provider: " + provider_id;
    }
    return false;
  }

  if (!ProviderHasModelId(*provider_info, model_id)) {
    DLOG(WARNING) << "Persisting Nexus default model not currently present in "
                  << "provider catalog: " << provider_id << "/" << model_id;
  }

  LlmProvider* provider = broker->GetProvider(provider_id);
  if (!provider) {
    if (error_message) {
      *error_message = "Provider is unavailable: " + provider_id;
    }
    return false;
  }

  const base::DictValue* stored_config = GetStoredProviderConfig(prefs, provider_id);
  base::DictValue config = stored_config ? stored_config->Clone() : base::DictValue();
  config.Set("default_model", model_id);
  config.Set("model", model_id);

  if (!broker->ConfigureProvider(provider_id, config)) {
    if (error_message) {
      *error_message = "Failed to configure provider: " + provider_id;
    }
    return false;
  }

  if (!broker->SetActiveProvider(provider_id)) {
    if (error_message) {
      *error_message = "Failed to set active provider: " + provider_id;
    }
    return false;
  }

  return true;
}

}  // namespace nexus
