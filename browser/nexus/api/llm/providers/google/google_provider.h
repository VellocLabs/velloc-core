#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_GOOGLE_GOOGLE_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_GOOGLE_GOOGLE_PROVIDER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace nexus {

class GoogleProvider : public LlmProvider {
 public:
  explicit GoogleProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
  ~GoogleProvider() override;

  GoogleProvider(const GoogleProvider&) = delete;
  GoogleProvider& operator=(const GoogleProvider&) = delete;

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

  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;
  // Fetch requests that arrived while models_loader_ was in flight; resolved
  // with the active fetch's result (see FetchAvailableModels).
  std::vector<FetchModelsCallback> pending_models_callbacks_;

  std::string api_key_;
  // Guest-supplied via SetDefaultBaseUrl(); empty until the catalog arrives.
  std::string default_base_url_;
  std::string base_url_;
  // True while `base_url_` is a user override rather than the default.
  bool base_url_overridden_ = false;
  std::string model_id_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<GoogleProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_GOOGLE_GOOGLE_PROVIDER_H_
