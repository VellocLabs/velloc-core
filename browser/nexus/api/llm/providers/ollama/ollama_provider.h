#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OLLAMA_OLLAMA_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OLLAMA_OLLAMA_PROVIDER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace nexus {

class OllamaProvider final : public LlmProvider {
 public:
  explicit OllamaProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
  ~OllamaProvider() override;

  OllamaProvider(const OllamaProvider&) = delete;
  OllamaProvider& operator=(const OllamaProvider&) = delete;

  // LlmProvider:
  std::string GetId() const override;
  std::string GetDisplayName() const override;
  LlmProviderType GetProviderType() const override;
  std::vector<ModelInfo> GetSupportedModels() const override;
  std::string GetDefaultModelId() const override;
  bool SupportsStreaming() const override;
  bool SupportsTools() const override;
  bool IsConfigured() const override;
  bool Configure(const base::DictValue& config) override;
  std::string GetDefaultBaseUrl() const override;
  void SetDefaultBaseUrl(const std::string& url) override;
  std::string GetBaseUrl() const override;

  void FetchAvailableModels(FetchModelsCallback callback) override;
  void LoadCachedCatalogJson(const std::string& raw_json) override;
  std::string GetLastFetchedRawCatalogJson() const override;

 private:
  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  // Guest-supplied via SetDefaultBaseUrl(); empty until the catalog arrives.
  std::string default_base_url_;
  std::string base_url_;
  // True while `base_url_` is a user override rather than the default.
  bool base_url_overridden_ = false;
  std::string configured_model_;
  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;
  // Fetch requests that arrived while models_loader_ was in flight; resolved
  // with the active fetch's result (see FetchAvailableModels).
  std::vector<FetchModelsCallback> pending_models_callbacks_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OllamaProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OLLAMA_OLLAMA_PROVIDER_H_
