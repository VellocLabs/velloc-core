// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_CHROMIUM_OVERRIDES_FEATURE_LIST_NAMES_H_
#define CUSTOM_BROWSER_CHROMIUM_OVERRIDES_FEATURE_LIST_NAMES_H_

#include <string>
#include <string_view>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace custom_browser {

// Whether `feature_list`, an --enable-features / --disable-features value,
// names `feature` exactly. An entry may start with '*' and end in a
// "<Study", ".Group" or ":params" suffix; neither is part of its name.
inline bool FeatureListNamesFeature(std::string_view feature_list,
                                    std::string_view feature) {
  for (std::string_view entry : base::SplitStringPiece(
           feature_list, ",", base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    if (entry.starts_with('*')) {
      entry.remove_prefix(1);
    }
    entry = entry.substr(0, entry.find_first_of("<.:"));
    if (entry == feature) {
      return true;
    }
  }
  return false;
}

// Upstream features pinned OFF by name through --disable-features, which
// wins over the source default, a field trial and --enable-features.
inline constexpr const char* kPinnedOffFeatureNames[] = {
    // Our search-engine UI relies on the legacy getSearchEnginesList()
    // message and /searchEngines route (CHECK crash).
    "SearchSettingsUpdate",
    // Gates Chrome's WebUI toolbar, a second always-visible top-chrome WebUI
    // that would share the chrome://custom.top-chrome shell renderer and hold
    // it at foreground priority while hidden (P-2).
    "InitialWebUI",
    // Its startup metrics task waits on another process for the OneDrive
    // folder's sync state; with no answer it holds browser shutdown open (P-5).
    "CloudSyncedFolderChecker",
};

// `disable_features`, a --disable-features value, extended so it names every
// pinned-off feature. Entries already present are kept as written; a value
// that already names them all is returned unchanged.
inline std::string WithPinnedOffFeatures(std::string disable_features) {
  for (const char* feature : kPinnedOffFeatureNames) {
    if (FeatureListNamesFeature(disable_features, feature)) {
      continue;
    }
    disable_features = disable_features.empty()
                           ? std::string(feature)
                           : std::string(feature) + "," + disable_features;
  }
  return disable_features;
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_OVERRIDES_FEATURE_LIST_NAMES_H_
