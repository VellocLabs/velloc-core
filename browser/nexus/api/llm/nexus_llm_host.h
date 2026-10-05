#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_llm.mojom.h"

class Profile;

namespace nexus {

// Serves the `model.catalog` and `model.setDefaultSelection` bridge actions
// from the broker's providers and the guest-published TS catalog.
//
// The inference half (`model.call` / `streamStart` / `streamRead` /
// `streamCancel`, which translated the bridge request into a provider call
// and queued its stream events for held reads) was removed under interface 1
// on 2026-10-04: the runtime speaks every provider's wire format over
// `model.rawStream*` / `network.stream*`.
class NexusLlmHost {
 public:
  using CatalogCallback =
      base::OnceCallback<void(bool success,
                              mojom::NexusLlmCatalogPtr catalog,
                              std::optional<std::string> error_message)>;

  explicit NexusLlmHost(Profile* profile);
  ~NexusLlmHost();

  NexusLlmHost(const NexusLlmHost&) = delete;
  NexusLlmHost& operator=(const NexusLlmHost&) = delete;

  void Catalog(CatalogCallback callback);
  bool SetDefaultSelection(const std::string& provider_id,
                           const std::string& model_id,
                           std::string* error_message);

 private:
  raw_ptr<Profile> profile_;
  base::TimeTicks last_catalog_refresh_time_;
  std::vector<std::string> last_catalog_refresh_provider_ids_;
  base::WeakPtrFactory<NexusLlmHost> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_
