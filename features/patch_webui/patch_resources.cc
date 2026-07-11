#include "custom_browser/features/patch_webui/patch_resources.h"

#include <string>
#include <string_view>

#include "chrome/grit/patch_webui_resources_map.h"
#include "content/public/browser/web_ui_data_source.h"
#include "custom_browser/common/pref_names.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"

namespace custom_browser::patch_webui {

namespace {

bool IsSharedMojomWebUiModule(std::string_view path) {
  return path.find('/') == std::string_view::npos &&
         path.ends_with(".mojom-webui.js");
}

void AddLocalizedModeStrings(content::WebUIDataSource* data_source) {
  data_source->AddLocalizedString("modesHeader",
                                  IDS_CUSTOM_BROWSER_MODES_HEADER);
  data_source->AddLocalizedString("modeSidebar",
                                  IDS_CUSTOM_BROWSER_MODE_SIDEBAR);
  data_source->AddLocalizedString("modeCompactSidebar",
                                  IDS_CUSTOM_BROWSER_MODE_COMPACT_SIDEBAR);
  data_source->AddLocalizedString("modeStandard",
                                  IDS_CUSTOM_BROWSER_MODE_STANDARD);
  data_source->AddLocalizedString("modeCompactStandard",
                                  IDS_CUSTOM_BROWSER_MODE_COMPACT_STANDARD);
  // kNexus is a runtime-only mode and is intentionally not offered as a
  // selectable UI-mode option, so no modeNexus* strings are injected.
  data_source->AddLocalizedString("modeSidebarDescription",
                                  IDS_CUSTOM_BROWSER_MODE_SIDEBAR_DESCRIPTION);
  data_source->AddLocalizedString(
      "modeCompactSidebarDescription",
      IDS_CUSTOM_BROWSER_MODE_COMPACT_SIDEBAR_DESCRIPTION);
  data_source->AddLocalizedString("modeStandardDescription",
                                  IDS_CUSTOM_BROWSER_MODE_STANDARD_DESCRIPTION);
  data_source->AddLocalizedString(
      "modeCompactStandardDescription",
      IDS_CUSTOM_BROWSER_MODE_COMPACT_STANDARD_DESCRIPTION);
}

}  // namespace

void AddPatchResourcesForHost(std::string_view host,
                              content::WebUIDataSource* data_source) {
  if (!data_source || host.empty()) {
    return;
  }

  for (const auto& item : kPatchWebuiResources) {
    const std::string_view path(item.path);
    const size_t host_pos = path.find(host);
    if (host_pos == std::string_view::npos && !IsSharedMojomWebUiModule(path)) {
      continue;
    }

    if (IsSharedMojomWebUiModule(path)) {
      data_source->AddResourcePath(path, item.id);
      continue;
    }

    // Register by file name (not the host-relative subdirectory path) so that
    // the settings loader's flat imports -- e.g. custom_main.ts does
    // `import('./about_patch.js')` for a module generated from
    // `settings/about/about_patch.js` -- resolve against
    // `chrome://settings/about_patch.js`. See PatchResourceRegistrationName.
    const std::string_view file_name = PatchResourceRegistrationName(path);
    if (file_name.empty()) {
      continue;
    }

    data_source->AddResourcePath(file_name, item.id);
  }
}

void AddCommonPatchStrings(content::WebUIDataSource* data_source) {
  data_source->AddString("customBrowserUIModePrefPath", kCustomUIModePrefPath);
  data_source->AddString("customNewTabShowSearchPrefPath",
                         kCustomNewTabShowSearchPrefPath);
  data_source->AddString("customNewTabShowLogoPrefPath",
                         kCustomNewTabShowLogoPrefPath);
  data_source->AddString("customNewTabShowShortcutsPrefPath",
                         kCustomNewTabShowShortcutsPrefPath);
  AddLocalizedModeStrings(data_source);
}

}  // namespace custom_browser::patch_webui
