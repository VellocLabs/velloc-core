#ifndef CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_
#define CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_

#include <functional>
#include <vector>

#include "base/feature_list.h"
#include "chrome/browser/ui/ui_features.h"

namespace custom_browser {

inline void AppendFeatureListOverrides(
    std::vector<base::FeatureList::FeatureOverrideInfo>* overrides) {
  if (!overrides) {
    return;
  }
  overrides->emplace_back(std::cref(features::kTabstripComboButton),
                          base::FeatureList::OVERRIDE_DISABLE_FEATURE);
  overrides->emplace_back(
      std::cref(features::kLaunchedTabSearchToolbarButton),
      base::FeatureList::OVERRIDE_DISABLE_FEATURE);
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_FEATURE_LIST_OVERRIDE_H_
