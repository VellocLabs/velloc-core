#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_SUPPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_SUPPORT_H_

#include "base/values.h"
#include "custom_browser/browser/nexus/mojom/nexus_llm.mojom.h"

namespace nexus {

// The `model.catalog` result in its bridge (camelCase) shape. The request /
// stream conversions that used to live here went with `model.call` /
// `streamStart` / `streamRead` (removed under interface 1 on 2026-10-04).
base::DictValue CatalogToBridge(const mojom::NexusLlmCatalog& catalog);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_SUPPORT_H_
