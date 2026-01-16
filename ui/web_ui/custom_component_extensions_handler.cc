#include "custom_browser/ui/web_ui/custom_component_extensions_handler.h"

#include <optional>
#include <utility>

#include "base/files/file_path.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/extensions/custom_component_extensions.h"
#include "chrome/browser/extensions/component_loader.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"

namespace custom_browser {

namespace {

void PinExtensionToToolbar(Profile* profile, const std::string& extension_id) {
  if (!profile || extension_id.empty()) {
    return;
  }

  ToolbarActionsModel* toolbar_model = ToolbarActionsModel::Get(profile);
  if (!toolbar_model) {
    return;
  }

  if (toolbar_model->IsActionForcePinned(extension_id) ||
      toolbar_model->IsActionPinned(extension_id)) {
    return;
  }

  toolbar_model->SetActionVisibility(extension_id, true);
}

}  // namespace

CustomComponentExtensionsHandler::CustomComponentExtensionsHandler(
    Profile* profile)
    : profile_(profile) {}

CustomComponentExtensionsHandler::~CustomComponentExtensionsHandler() = default;

void CustomComponentExtensionsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getCustomComponentExtensions",
      base::BindRepeating(
          &CustomComponentExtensionsHandler::HandleGetCustomComponentExtensions,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setCustomComponentExtensionEnabled",
      base::BindRepeating(
          &CustomComponentExtensionsHandler::
              HandleSetCustomComponentExtensionEnabled,
          base::Unretained(this)));
}

void CustomComponentExtensionsHandler::HandleGetCustomComponentExtensions(
    const base::Value::List& args) {
  AllowJavascript();
  CHECK_EQ(args.size(), 1u);
  const base::Value& callback_id = args[0];

  base::Value::List result;
  if (profile_) {
    for (const auto& info :
         GetCustomComponentExtensionInfos(profile_->GetPrefs())) {
      base::Value::Dict entry;
      entry.Set("name", info.name);
      entry.Set("displayName", info.display_name);
      if (!info.description.empty()) {
        entry.Set("description", info.description);
      }
      if (!info.version.empty()) {
        entry.Set("version", info.version);
      }
      if (!info.extension_id.empty()) {
        entry.Set("id", info.extension_id);
      }
      entry.Set("enabled", info.enabled);
      entry.Set("enabledByDefault", info.enabled_by_default);
      entry.Set("hasBackgroundPage", info.has_background_page);
      result.Append(std::move(entry));
    }
  }

  ResolveJavascriptCallback(callback_id, result);
}

void CustomComponentExtensionsHandler::HandleSetCustomComponentExtensionEnabled(
    const base::Value::List& args) {
  AllowJavascript();
  CHECK_EQ(args.size(), 3u);
  const base::Value& callback_id = args[0];

  const std::string* name = args[1].GetIfString();
  const std::optional<bool> enabled = args[2].GetIfBool();
  bool success = false;

  if (profile_ && name && enabled.has_value()) {
    const CustomComponentExtension* entry =
        FindCustomComponentExtensionByName(*name);
    if (entry) {
      SetCustomComponentExtensionEnabled(profile_->GetPrefs(), *entry,
                                         *enabled);
      extensions::ComponentLoader* loader =
          extensions::ComponentLoader::Get(profile_);
      if (loader) {
        const std::string extension_id = GetCustomComponentExtensionId(*entry);
        std::string loaded_id = extension_id;
        if (*enabled) {
          if (!extension_id.empty() && loader->Exists(extension_id)) {
            success = true;
          } else {
            loaded_id =
                loader->Add(entry->manifest_resource_id,
                            base::FilePath(entry->root_relative_path));
            success = !loaded_id.empty();
          }
          if (success) {
            PinExtensionToToolbar(profile_, loaded_id);
          }
        } else {
          if (!extension_id.empty()) {
            loader->Remove(extension_id);
          } else {
            loader->Remove(base::FilePath(entry->root_relative_path));
          }
          success = true;
        }
      }
    }
  }

  ResolveJavascriptCallback(callback_id, base::Value(success));
}

}  // namespace custom_browser
