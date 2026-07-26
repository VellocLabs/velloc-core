#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace nexus {

// Abstract base class for all LLM providers. Each concrete provider translates
// from the Vercel-aligned common wire format to its native API format and back.
//
// Lifecycle:
//   1. Constructed by LlmProviderFactory with a SharedURLLoaderFactory.
//   2. Configure() is called with API key / base URL from prefs.
//   3. FetchAvailableModels() is called to populate the dynamic model list.
//   4. DoGenerate() / DoStream() handle inference requests.
class LlmProvider {
 public:
  virtual ~LlmProvider() = default;

  LlmProvider(const LlmProvider&) = delete;
  LlmProvider& operator=(const LlmProvider&) = delete;

  // --- Identity ---
  virtual std::string GetId() const = 0;
  virtual std::string GetDisplayName() const = 0;
  virtual LlmProviderType GetProviderType() const = 0;

  // --- Model catalogue ---

  // Returns the current model list. If a catalogue has been loaded (either
  // via FetchAvailableModels or LoadCachedCatalogJson) this returns that
  // list; otherwise returns an empty vector.
  virtual std::vector<ModelInfo> GetSupportedModels() const = 0;

  virtual std::string GetDefaultModelId() const = 0;

  // Asynchronously fetches the model catalogue from the provider's API
  // (e.g. GET /v1/models). On success the fetched list replaces whatever
  // was previously loaded. On failure the callback receives an empty list.
  // Default implementation invokes |callback| with GetSupportedModels().
  virtual void FetchAvailableModels(FetchModelsCallback callback);

  // Re-populates the in-memory model list from a raw /v1/models response
  // body that was persisted by LlmBroker on a previous run. No network
  // activity. Retained so Settings-UI-triggered refreshes can still round-
  // trip a fetched catalogue through prefs; the authoritative model list
  // shown to users lives in the TS config, not here. Default is a no-op.
  virtual void LoadCachedCatalogJson(const std::string& raw_json);

  // Returns the raw response body from the most recent successful
  // FetchAvailableModels call, or the cached JSON if LoadCachedCatalogJson
  // was the last thing that populated the model list. Empty string if
  // neither has happened yet.
  virtual std::string GetLastFetchedRawCatalogJson() const;

  // --- Capabilities ---
  virtual bool SupportsStreaming() const = 0;
  virtual bool SupportsTools() const = 0;

  // --- Configuration ---
  virtual bool IsConfigured() const = 0;
  virtual bool Configure(const base::DictValue& config) = 0;

  // Returns the provider's built-in default base URL.
  virtual std::string GetDefaultBaseUrl() const = 0;

  // Returns the currently active base URL (custom if set, otherwise default).
  virtual std::string GetBaseUrl() const = 0;

  // --- Inference ---

  // Non-streaming generation. |call_options| is a Vercel-aligned Dict flowing
  // directly from the TypeScript bridge.
  virtual void DoGenerate(const base::DictValue& call_options,
                          GenerateCallback callback) = 0;

  // Streaming generation.
  virtual void DoStream(const base::DictValue& call_options,
                        StreamChunkCallback chunk_callback,
                        StreamDoneCallback done_callback) = 0;

  virtual void CancelRequest(const std::string& request_id) = 0;

  // True when `request_id` has been accepted but is still waiting for a
  // concurrency slot — no upstream socket was ever opened, so it cannot have
  // produced bytes and MUST NOT be judged by an idle/silence deadline. Only
  // providers that implement a concurrency pool (today: OpenAI, via
  // LlmStreamGuard) can answer yes; the default is "not queued", which
  // preserves the previous behaviour for every other provider.
  virtual bool IsRequestQueued(const std::string& request_id) const;

 protected:
  LlmProvider() = default;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_H_
