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
  std::string GetBaseUrl() const override;
  void DoGenerate(const base::DictValue& call_options,
                  GenerateCallback callback) override;
  void DoStream(const base::DictValue& call_options,
                StreamChunkCallback chunk_callback,
                StreamDoneCallback done_callback) override;
  void CancelRequest(const std::string& request_id) override;
  void FetchAvailableModels(FetchModelsCallback callback) override;
  void LoadCachedCatalogJson(const std::string& raw_json) override;
  std::string GetLastFetchedRawCatalogJson() const override;

 private:
  class StreamHandler;

  struct PendingGenerate {
    PendingGenerate();
    ~PendingGenerate();
    PendingGenerate(PendingGenerate&&);
    PendingGenerate& operator=(PendingGenerate&&);
    PendingGenerate(const PendingGenerate&) = delete;
    PendingGenerate& operator=(const PendingGenerate&) = delete;

    std::unique_ptr<network::SimpleURLLoader> loader;
    GenerateCallback callback;
    base::DictValue request_body;
  };

  GURL BuildEndpointUrl(const std::string& model_id, bool streaming) const;
  base::DictValue BuildRequestBody(
      const base::DictValue& call_options) const;
  GenerateResult ParseResponse(const base::DictValue& response) const;
  std::unique_ptr<network::SimpleURLLoader> CreateLoader(
      const GURL& url,
      const std::string& body) const;

  void OnGenerateComplete(const std::string& request_id,
                          std::optional<std::string> response_body);

  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;

  std::string api_key_;
  std::string base_url_;
  std::string model_id_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::map<std::string, PendingGenerate> pending_generates_;
  std::map<std::string, std::unique_ptr<StreamHandler>> pending_streams_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<GoogleProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_GOOGLE_GOOGLE_PROVIDER_H_
