#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace nexus {

ModelInfo::ModelInfo() = default;
ModelInfo::ModelInfo(const ModelInfo&) = default;
ModelInfo::ModelInfo(ModelInfo&&) = default;
ModelInfo& ModelInfo::operator=(const ModelInfo&) = default;
ModelInfo& ModelInfo::operator=(ModelInfo&&) = default;
ModelInfo::~ModelInfo() = default;

ProviderInfo::ProviderInfo() = default;
ProviderInfo::ProviderInfo(const ProviderInfo&) = default;
ProviderInfo& ProviderInfo::operator=(const ProviderInfo&) = default;
ProviderInfo::~ProviderInfo() = default;

}  // namespace nexus
