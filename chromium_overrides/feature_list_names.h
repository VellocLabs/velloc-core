// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_CHROMIUM_OVERRIDES_FEATURE_LIST_NAMES_H_
#define CUSTOM_BROWSER_CHROMIUM_OVERRIDES_FEATURE_LIST_NAMES_H_

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

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_OVERRIDES_FEATURE_LIST_NAMES_H_
