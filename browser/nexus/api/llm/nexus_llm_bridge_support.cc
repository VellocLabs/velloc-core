#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_support.h"

#include <utility>

#include "custom_browser/browser/nexus/mojom/nexus_llm.mojom.h"

namespace nexus {

namespace {

using Dict = base::DictValue;
using List = base::ListValue;

}  // namespace

base::DictValue CatalogToBridge(const mojom::NexusLlmCatalog& catalog) {
  Dict result;
  List providers;

  for (const auto& provider : catalog.providers) {
    if (!provider) {
      continue;
    }

    Dict provider_dict;
    provider_dict.Set("id", provider->id);
    provider_dict.Set("label", provider->label);
    provider_dict.Set("iconSvg", provider->icon_svg);
    provider_dict.Set("enabled", provider->enabled);
    if (provider->default_model_id.has_value() &&
        !provider->default_model_id->empty()) {
      provider_dict.Set("defaultModelId", *provider->default_model_id);
    }

    List models;
    for (const auto& model : provider->models) {
      if (!model) {
        continue;
      }

      Dict model_dict;
      model_dict.Set("id", model->id);
      model_dict.Set("label", model->label);
      model_dict.Set("enabled", model->enabled);
      model_dict.Set("supportsStreaming", model->supports_streaming);
      model_dict.Set("supportsTools", model->supports_tools);
      if (model->default_thinking_tier.has_value() &&
          !model->default_thinking_tier->empty()) {
        model_dict.Set("defaultThinkingTier", *model->default_thinking_tier);
      }
      if (!model->available_thinking_tiers.empty()) {
        List tiers;
        for (const auto& tier : model->available_thinking_tiers) {
          tiers.Append(tier);
        }
        model_dict.Set("availableThinkingTiers", std::move(tiers));
      }
      models.Append(std::move(model_dict));
    }

    provider_dict.Set("models", std::move(models));
    providers.Append(std::move(provider_dict));
  }

  result.Set("providers", std::move(providers));

  if (catalog.default_selection) {
    Dict selection;
    selection.Set("providerId", catalog.default_selection->provider_id);
    selection.Set("modelId", catalog.default_selection->model_id);
    result.Set("defaultSelection", std::move(selection));
  }

  if (!catalog.raw_catalogs.empty()) {
    Dict raw_catalogs;
    for (const auto& [provider_id, raw] : catalog.raw_catalogs) {
      raw_catalogs.Set(provider_id, raw);
    }
    result.Set("rawCatalogs", std::move(raw_catalogs));
  }

  return result;
}

}  // namespace nexus
