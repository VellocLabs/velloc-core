#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_

#include <string>

#include "base/values.h"

namespace nexus {

base::DictValue BuildNexusHostManifest();
std::string BuildNexusBridgeBootstrapMessage();

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
