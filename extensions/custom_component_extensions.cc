#include "custom_browser/extensions/custom_component_extensions.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "chrome/grit/custom_component_extension_resources.h"
#include "chrome/grit/custom_component_extension_resources_map.h"
#include "components/crx_file/id_util.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "custom_browser/common/pref_names.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_l10n_util.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/message_bundle.h"
#include "ui/base/resource/resource_bundle.h"

namespace custom_browser {

namespace {

const std::vector<CustomComponentExtension>& GetCustomComponentExtensionsStorage() {
  static const base::NoDestructor<std::vector<CustomComponentExtension>>
      extensions(std::vector<CustomComponentExtension>{
#include "custom_browser/extensions/custom_component_extensions.inc"
      });
  return *extensions;
}

int GetResourceIdForCustomComponentPath(const base::FilePath& resource_path) {
  const std::string resource_path_string =
      resource_path.NormalizePathSeparatorsTo(FILE_PATH_LITERAL('/'))
          .AsUTF8Unsafe();
  for (const auto& entry : kCustomComponentExtensionResources) {
    if (resource_path_string == entry.path) {
      return entry.id;
    }
  }
  return 0;
}

std::optional<base::DictValue> ParseManifestResource(
    int manifest_resource_id) {
  if (!ui::ResourceBundle::HasSharedInstance()) {
    return std::nullopt;
  }

  std::string manifest_contents =
      ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
          manifest_resource_id);
  auto manifest = base::JSONReader::ReadDict(
      manifest_contents, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!manifest) {
    LOG(ERROR) << "Failed to parse custom component extension manifest (id="
               << manifest_resource_id << ").";
  }
  return manifest;
}

std::optional<base::DictValue> LoadMessageCatalogResource(
    const base::FilePath& extension_root,
    const std::string& locale) {
  if (!ui::ResourceBundle::HasSharedInstance()) {
    return std::nullopt;
  }

  base::FilePath resource_path = extension_root.Append(
      extensions::kLocaleFolder);
  resource_path = resource_path.AppendASCII(locale)
                      .Append(extensions::kMessagesFilename);
  resource_path = resource_path.NormalizePathSeparators();

  const int resource_id = GetResourceIdForCustomComponentPath(resource_path);
  if (!resource_id) {
    return std::nullopt;
  }

  std::string messages =
      ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
          resource_id);
  auto catalog = base::JSONReader::ReadDict(
      messages, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!catalog) {
    LOG(ERROR) << "Failed to parse messages.json for custom component extension "
               << "resource: " << resource_path.AsUTF8Unsafe();
  }
  return catalog;
}

// Locales to search for messages, most specific first: the application
// locale and its parents (e.g. "en_GB", "en"), then `default_locale`.
std::vector<std::string> GetFallbackLocales(const std::string& default_locale) {
  std::vector<std::string> locales;
  std::string tag = extension_l10n_util::CurrentLocaleOrDefault();
  while (!tag.empty() && tag != default_locale) {
    locales.push_back(tag);
    const size_t separator = tag.find_last_of("_-");
    if (separator == std::string::npos) {
      break;
    }
    tag.resize(separator);
  }
  locales.push_back(default_locale);
  return locales;
}

std::optional<base::DictValue> LocalizeManifestForDisplay(
    const CustomComponentExtension& extension,
    const base::DictValue& manifest) {
  std::string error;
  const std::string default_locale =
      extension_l10n_util::GetDefaultLocaleFromManifest(manifest, &error);
  if (default_locale.empty()) {
    return std::nullopt;
  }

  const std::vector<std::string> fallback_locales =
      GetFallbackLocales(default_locale);

  extensions::MessageBundle::CatalogVector catalogs;
  const base::FilePath extension_root(extension.root_relative_path);
  for (const auto& locale : fallback_locales) {
    std::optional<base::DictValue> catalog =
        LoadMessageCatalogResource(extension_root, locale);
    if (catalog) {
      catalogs.push_back(std::move(*catalog));
    }
  }

  if (catalogs.empty()) {
    return std::nullopt;
  }

  std::unique_ptr<extensions::MessageBundle> bundle(
      extensions::MessageBundle::Create(catalogs, &error));
  if (!bundle) {
    LOG(ERROR) << "Failed to parse localization catalogs for custom component "
               << "extension " << extension.name << ": " << error;
    return std::nullopt;
  }

  base::DictValue localized_manifest = manifest.Clone();
  if (!extension_l10n_util::LocalizeManifest(*bundle, &localized_manifest,
                                             &error)) {
    LOG(ERROR) << "Failed to localize manifest for custom component extension "
               << extension.name << ": " << error;
    return std::nullopt;
  }

  return localized_manifest;
}

extensions::ExtensionId GenerateIdFromManifest(const base::DictValue& manifest,
                                               int manifest_resource_id) {
  const std::string* raw_key =
      manifest.FindString(extensions::manifest_keys::kPublicKey);
  if (!raw_key) {
    LOG(ERROR) << "Missing manifest key in custom component extension (id="
               << manifest_resource_id << ").";
    return std::string();
  }

  std::string id_input;
  if (!extensions::Extension::ParsePEMKeyBytes(*raw_key, &id_input)) {
    LOG(ERROR) << "Failed to parse manifest key in custom component extension "
               << "(id=" << manifest_resource_id << ").";
    return std::string();
  }

  return crx_file::id_util::GenerateId(id_input);
}

const extensions::ExtensionIdSet& GetCustomExtensionIdAllowlist() {
  static base::NoDestructor<extensions::ExtensionIdSet> allowlist;
  static bool initialized = false;

  if (initialized) {
    return *allowlist;
  }

  if (!ui::ResourceBundle::HasSharedInstance()) {
    return *allowlist;
  }

  for (const auto& entry : GetCustomComponentExtensionsStorage()) {
    std::optional<base::DictValue> manifest =
        ParseManifestResource(entry.manifest_resource_id);
    if (!manifest) {
      continue;
    }

    extensions::ExtensionId extension_id =
        GenerateIdFromManifest(*manifest, entry.manifest_resource_id);
    if (!extension_id.empty()) {
      allowlist->insert(std::move(extension_id));
    }
  }

  initialized = true;
  return *allowlist;
}

bool ReadEnabledPref(const PrefService* prefs,
                     const CustomComponentExtension& extension,
                     bool* has_pref) {
  if (has_pref) {
    *has_pref = false;
  }
  if (!prefs) {
    return extension.enabled_by_default;
  }
  const base::DictValue& dict =
      prefs->GetDict(kCustomComponentExtensionsPrefPath);
  const base::Value* value = dict.Find(extension.name);
  if (!value || !value->is_bool()) {
    return extension.enabled_by_default;
  }
  if (has_pref) {
    *has_pref = true;
  }
  return value->GetBool();
}

}  // namespace

CustomComponentExtensionInfo::CustomComponentExtensionInfo() = default;
CustomComponentExtensionInfo::CustomComponentExtensionInfo(
    const CustomComponentExtensionInfo&) = default;
CustomComponentExtensionInfo& CustomComponentExtensionInfo::operator=(
    const CustomComponentExtensionInfo&) = default;
CustomComponentExtensionInfo::CustomComponentExtensionInfo(
    CustomComponentExtensionInfo&&) = default;
CustomComponentExtensionInfo& CustomComponentExtensionInfo::operator=(
    CustomComponentExtensionInfo&&) = default;
CustomComponentExtensionInfo::~CustomComponentExtensionInfo() = default;

base::span<const CustomComponentExtension> GetCustomComponentExtensions() {
  return base::span<const CustomComponentExtension>(
      GetCustomComponentExtensionsStorage());
}

bool IsCustomComponentExtensionAllowlisted(const std::string& extension_id) {
  const auto& allowlist = GetCustomExtensionIdAllowlist();
  return allowlist.contains(extension_id);
}

bool IsCustomComponentExtensionAllowlisted(int manifest_resource_id) {
  for (const auto& entry : GetCustomComponentExtensionsStorage()) {
    if (entry.manifest_resource_id == manifest_resource_id) {
      return true;
    }
  }
  return false;
}

const CustomComponentExtension* FindCustomComponentExtensionByManifestResourceId(
    int manifest_resource_id) {
  for (const auto& entry : GetCustomComponentExtensionsStorage()) {
    if (entry.manifest_resource_id == manifest_resource_id) {
      return &entry;
    }
  }
  return nullptr;
}

const CustomComponentExtension* FindCustomComponentExtensionByName(
    std::string_view name) {
  for (const auto& entry : GetCustomComponentExtensionsStorage()) {
    if (name == entry.name) {
      return &entry;
    }
  }
  return nullptr;
}

bool IsCustomComponentExtensionEnabled(
    PrefService* prefs,
    const CustomComponentExtension& extension) {
  return ReadEnabledPref(prefs, extension, nullptr);
}

void SetCustomComponentExtensionEnabled(
    PrefService* prefs,
    const CustomComponentExtension& extension,
    bool enabled) {
  if (!prefs) {
    return;
  }
  ScopedDictPrefUpdate update(prefs, kCustomComponentExtensionsPrefPath);
  update->Set(extension.name, enabled);
}

std::string GetCustomComponentExtensionId(
    const CustomComponentExtension& extension) {
  std::optional<base::DictValue> manifest =
      ParseManifestResource(extension.manifest_resource_id);
  if (!manifest) {
    return std::string();
  }
  return GenerateIdFromManifest(*manifest, extension.manifest_resource_id);
}

std::vector<CustomComponentExtensionInfo> GetCustomComponentExtensionInfos(
    PrefService* prefs) {
  std::vector<CustomComponentExtensionInfo> result;
  for (const auto& entry : GetCustomComponentExtensionsStorage()) {
    CustomComponentExtensionInfo info;
    info.name = entry.name;
    info.enabled_by_default = entry.enabled_by_default;
    info.has_background_page = entry.has_background_page;
    info.enabled = ReadEnabledPref(prefs, entry, nullptr);

    std::optional<base::DictValue> manifest =
        ParseManifestResource(entry.manifest_resource_id);
    if (manifest) {
      std::optional<base::DictValue> localized_manifest =
          LocalizeManifestForDisplay(entry, *manifest);
      const base::DictValue& info_manifest =
          localized_manifest ? *localized_manifest : *manifest;

      if (const std::string* name =
              info_manifest.FindString(extensions::manifest_keys::kName)) {
        info.display_name = *name;
      }
      if (const std::string* description =
              info_manifest.FindString(
                  extensions::manifest_keys::kDescription)) {
        info.description = *description;
      }
      if (const std::string* version =
              info_manifest.FindString(extensions::manifest_keys::kVersion)) {
        info.version = *version;
      }
      info.extension_id =
          GenerateIdFromManifest(*manifest, entry.manifest_resource_id);
    }

    if (info.display_name.empty()) {
      info.display_name = entry.name;
    }

    result.push_back(std::move(info));
  }
  return result;
}

bool LocalizeCustomComponentManifest(int manifest_resource_id,
                                     base::DictValue* manifest) {
  if (!manifest) {
    return false;
  }
  const CustomComponentExtension* extension =
      FindCustomComponentExtensionByManifestResourceId(manifest_resource_id);
  if (!extension) {
    return false;
  }
  std::optional<base::DictValue> localized_manifest =
      LocalizeManifestForDisplay(*extension, *manifest);
  if (!localized_manifest) {
    return false;
  }
  *manifest = std::move(*localized_manifest);
  return true;
}

}  // namespace custom_browser
