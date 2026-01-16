#ifndef CUSTOM_BROWSER_EXTENSIONS_CUSTOM_COMPONENT_EXTENSIONS_H_
#define CUSTOM_BROWSER_EXTENSIONS_CUSTOM_COMPONENT_EXTENSIONS_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/values.h"

class PrefService;

namespace custom_browser {

struct CustomComponentExtension {
  const char* name;
  int manifest_resource_id;
  const base::FilePath::CharType* root_relative_path;
  bool has_background_page;
  bool enabled_by_default;
};

struct CustomComponentExtensionInfo {
  CustomComponentExtensionInfo();
  CustomComponentExtensionInfo(const CustomComponentExtensionInfo&);
  CustomComponentExtensionInfo& operator=(const CustomComponentExtensionInfo&);
  CustomComponentExtensionInfo(CustomComponentExtensionInfo&&);
  CustomComponentExtensionInfo& operator=(CustomComponentExtensionInfo&&);
  ~CustomComponentExtensionInfo();

  std::string name;
  std::string display_name;
  std::string description;
  std::string version;
  std::string extension_id;
  bool enabled;
  bool enabled_by_default;
  bool has_background_page;
};

base::span<const CustomComponentExtension> GetCustomComponentExtensions();
bool IsCustomComponentExtensionAllowlisted(const std::string& extension_id);
bool IsCustomComponentExtensionAllowlisted(int manifest_resource_id);
const CustomComponentExtension* FindCustomComponentExtensionByName(
    std::string_view name);
bool IsCustomComponentExtensionEnabled(
    PrefService* prefs,
    const CustomComponentExtension& extension);
void SetCustomComponentExtensionEnabled(
    PrefService* prefs,
    const CustomComponentExtension& extension,
    bool enabled);
std::string GetCustomComponentExtensionId(
    const CustomComponentExtension& extension);
std::vector<CustomComponentExtensionInfo> GetCustomComponentExtensionInfos(
    PrefService* prefs);
bool LocalizeCustomComponentManifest(int manifest_resource_id,
                                     base::Value::Dict* manifest);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_EXTENSIONS_CUSTOM_COMPONENT_EXTENSIONS_H_
