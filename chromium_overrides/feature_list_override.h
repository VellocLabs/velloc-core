#ifndef CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_
#define CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_

#include <functional>
#include <vector>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"
#include "components/actor/core/actor_features.h"

namespace custom_browser {

inline void AppendFeatureListOverrides(
    std::vector<base::FeatureList::FeatureOverrideInfo>* overrides) {
  if (!overrides) {
    return;
  }

  // Force a policy exemption for Glic Actor so web actuation settings can be
  // enabled in non-official builds without Chrome sign-in.
  {
    constexpr char kGlicActorPolicyExemptionFeature[] =
        "GlicActor:glic_actor_policy_control_exemption/true";
    auto* command_line = base::CommandLine::ForCurrentProcess();
    std::string enable_features =
        command_line->GetSwitchValueASCII(::switches::kEnableFeatures);
    if (enable_features.find(kGlicActorPolicyExemptionFeature) ==
        std::string::npos) {
      if (enable_features.empty()) {
        enable_features = kGlicActorPolicyExemptionFeature;
      } else {
        enable_features = std::string(kGlicActorPolicyExemptionFeature) + "," +
                          enable_features;
      }
      command_line->AppendSwitchASCII(::switches::kEnableFeatures,
                                      enable_features);
    }
  }

  // Ensure GLIC request headers are enabled so backend routing can attach the
  // browser-specific toolset.
  overrides->emplace_back(std::cref(features::kGlicHeader),
                          base::FeatureList::OVERRIDE_ENABLE_FEATURE);
  // Ensure actor APIs are available for web actuation.
  overrides->emplace_back(std::cref(features::kGlicActor),
                          base::FeatureList::OVERRIDE_ENABLE_FEATURE);

  // Ensure the web actuation toggle is visible regardless of pref state.
  {
    auto* command_line = base::CommandLine::ForCurrentProcess();
    if (!command_line->HasSwitch(::switches::kGlicAlwaysShowWebActuationToggle)) {
      command_line->AppendSwitch(::switches::kGlicAlwaysShowWebActuationToggle);
    }
  }

  // Enable development cookie sync for GLIC so the webview can authenticate
  // without official Google API keys.
  overrides->emplace_back(
      std::cref(features::kGlicDevelopmentSyncGoogleCookies),
      base::FeatureList::OVERRIDE_ENABLE_FEATURE);

  // Let the agent's tool-triggered navigations (e.g. a clicked search result)
  // cross origins; the actor's navigation gate would otherwise block them.
  // Trade-off: this also drops the sensitive-URL confirmation for actor
  // navigations.
  overrides->emplace_back(std::cref(actor::kGlicCrossOriginNavigationGating),
                          base::FeatureList::OVERRIDE_DISABLE_FEATURE);

  // Pin the File System Access picker block ON: an agent tab has no human to
  // answer a native picker, and the agent uses BrowserUpload / BrowserDownload.
  // Pinned so a default flip cannot reintroduce a blocking native dialog.
  overrides->emplace_back(
      std::cref(actor::kGlicBlockFileSystemAccessApiFilePicker),
      base::FeatureList::OVERRIDE_ENABLE_FEATURE);

  // Pinned OFF via --disable-features: the pin wins over field trials and
  // drops every study that carries the feature.
  // - "SearchSettingsUpdate": our search-engine UI relies on the legacy
  //   getSearchEnginesList() message and /searchEngines route (CHECK crash).
  // - "InitialWebUI": gates Chrome's WebUI toolbar, a second always-visible
  //   top-chrome WebUI that would share the chrome://custom.top-chrome shell
  //   renderer and hold it at foreground priority while hidden (P-2).
  {
    constexpr const char* kPinnedOffFeatures[] = {"SearchSettingsUpdate",
                                                  "InitialWebUI"};
    auto* command_line = base::CommandLine::ForCurrentProcess();
    std::string disable_features =
        command_line->GetSwitchValueASCII(::switches::kDisableFeatures);
    bool changed = false;
    for (const char* feature : kPinnedOffFeatures) {
      if (disable_features.find(feature) != std::string::npos) {
        continue;
      }
      disable_features = disable_features.empty()
                             ? std::string(feature)
                             : std::string(feature) + "," + disable_features;
      changed = true;
    }
    if (changed) {
      command_line->AppendSwitchASCII(::switches::kDisableFeatures,
                                      disable_features);
    }
  }
}
// HasTabSearchToolbarButton
}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_
