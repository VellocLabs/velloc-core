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

  if (!glic::GlicEnabling::IsEnabledByFlags()) {
    // kTabstripComboButton and kLaunchedTabSearchToolbarButton were removed in
    // Chromium 146. No overrides needed.
  }

  // Enable development cookie sync for GLIC so the webview can authenticate
  // without official Google API keys.
  overrides->emplace_back(
      std::cref(features::kGlicDevelopmentSyncGoogleCookies),
      base::FeatureList::OVERRIDE_ENABLE_FEATURE);

  // Let the agent's tool-triggered navigations cross origins freely — e.g.
  // clicking a Google/Bing search result that leads to another site. The
  // actor's cross-origin navigation gate (`IsNavigationGatingEnabled()` →
  // `kGlicCrossOriginNavigationGating`, see
  // components/actor/core/actor_util.cc + chrome/browser/actor/
  // execution_engine.cc) otherwise fails such a click with
  // `kTriggeredNavigationBlocked` because a click's destination origin is
  // never auto-granted (only an explicit Navigate grants its target origin).
  //
  // NOTE: the `confirm_navigation_to_new_origins` param is NOT the lever — it
  // already defaults to false (= allow novel non-sensitive origins). The
  // effective gate is the parent feature, so we disable it WHOLESALE here.
  // Trade-off: this also drops the sensitive-URL confirmation step for actor
  // navigations — accepted so clicked links can navigate anywhere without a
  // prompt the agent can't answer.
  overrides->emplace_back(std::cref(actor::kGlicCrossOriginNavigationGating),
                          base::FeatureList::OVERRIDE_DISABLE_FEATURE);

  // Pin the File System Access picker block ON. On an agent-controlled tab
  // there is no human to answer a native picker, so showOpenFilePicker /
  // showSaveFilePicker / showDirectoryPicker must be denied — the agent handles
  // files through BrowserUpload / BrowserDownload instead. Upstream's
  // IsFileSystemAccessApiFilePickerAllowed already does this while the actor is
  // acting (and FSA APIs require transient activation, which on an agent tab
  // only exists during actor actuation — so the acting-window check fully
  // covers the reachable cases). It is enabled by default upstream; we pin it
  // so a future default flip or field-trial config can't silently reintroduce
  // a blocking native dialog. This keeps the fix in our layer with no upstream
  // edit.
  // (Point B).
  overrides->emplace_back(
      std::cref(actor::kGlicBlockFileSystemAccessApiFilePicker),
      base::FeatureList::OVERRIDE_ENABLE_FEATURE);

  // Pin the Chromium 149 "SearchSettingsUpdate" search-settings rework OFF.
  // The custom browser's search-engine UI is built on the legacy model: the Get
  // Started default-engine picker and the customize-chrome search chip rely on
  // the getSearchEnginesList() WebUI message and the
  // chrome://settings/searchEngines route. SearchSettingsUpdate is
  // DISABLED_BY_DEFAULT upstream, but the field-trial testing config enables it
  // in dev builds -- and when enabled, getSearchEnginesList() CHECK-crashes the
  // browser process (search_engines_handler.cc) while route.ts drops the
  // /searchEngines route, breaking both surfaces. Append to --disable-features
  // (read by InitializeFromCommandLine before field trials, so it wins over the
  // testing config) rather than emplace_back(), whose precedence vs. an already
  // registered field-trial override is registration-order dependent.
  {
    constexpr char kSearchSettingsUpdateFeature[] = "SearchSettingsUpdate";
    auto* command_line = base::CommandLine::ForCurrentProcess();
    std::string disable_features =
        command_line->GetSwitchValueASCII(::switches::kDisableFeatures);
    if (disable_features.find(kSearchSettingsUpdateFeature) ==
        std::string::npos) {
      if (disable_features.empty()) {
        disable_features = kSearchSettingsUpdateFeature;
      } else {
        disable_features = std::string(kSearchSettingsUpdateFeature) + "," +
                           disable_features;
      }
      command_line->AppendSwitchASCII(::switches::kDisableFeatures,
                                      disable_features);
    }
  }
}
// HasTabSearchToolbarButton
}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_
