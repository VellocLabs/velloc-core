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

  if (!glic::GlicEnabling::IsEnabledByFlags()) {
    // kTabstripComboButton and kLaunchedTabSearchToolbarButton were removed in
    // Chromium 146. No overrides needed.
  }

  // Enable development cookie sync for GLIC so the webview can authenticate
  // without official Google API keys.
  overrides->emplace_back(
      std::cref(features::kGlicDevelopmentSyncGoogleCookies),
      base::FeatureList::OVERRIDE_ENABLE_FEATURE);

}
// HasTabSearchToolbarButton
}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_
