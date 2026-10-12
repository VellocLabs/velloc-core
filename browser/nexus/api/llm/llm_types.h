#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_TYPES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_TYPES_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"

namespace nexus {

// Provider identity and model-discovery types. The request / result / stream
// types of the C++ inference path (CallOptions, GenerateResult, StreamPart,
// the generate/stream callbacks, FinishReason) were removed with model.call /
// streamStart under interface 1.

// Provider classification.
enum class LlmProviderType {
  kCloud,
  kLocal,
  kProxy,
};

// Metadata for a single model offered by a provider.
struct ModelInfo {
  ModelInfo();
  ModelInfo(const ModelInfo&);
  ModelInfo(ModelInfo&&);
  ModelInfo& operator=(const ModelInfo&);
  ModelInfo& operator=(ModelInfo&&);
  ~ModelInfo();

  std::string id;
  std::string name;
  int context_window = 0;
  bool supports_tools = false;
  bool supports_vision = false;
  // Reasoning-effort metadata, populated by providers that discover it (e.g.
  // the Codex /models endpoint returns default + supported reasoning levels).
  // Empty for providers that don't expose it.
  std::optional<std::string> default_thinking_tier;
  std::vector<std::string> available_thinking_tiers;
};

// Metadata for a registered provider, including its model catalogue.
struct ProviderInfo {
  ProviderInfo();
  ProviderInfo(const ProviderInfo&);
  ProviderInfo& operator=(const ProviderInfo&);
  ~ProviderInfo();

  std::string id;
  std::string name;
  LlmProviderType type = LlmProviderType::kCloud;
  bool is_configured = false;
  std::string base_url;
  std::string default_base_url;
  std::vector<ModelInfo> models;
};

// Called when a dynamic model-list fetch completes.
using FetchModelsCallback =
    base::OnceCallback<void(std::vector<ModelInfo> models)>;

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_TYPES_H_
