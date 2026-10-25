// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_UTILITY_TAB_OPTIONS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_UTILITY_TAB_OPTIONS_H_

#include <string_view>

class GURL;

namespace nexus {

// The checks the `ui.openUtilityTab` family applies before touching a tab
// strip (F-35). Pure functions, UI thread or any.

// A utility tab's key: 1-32 characters of a-z, 0-9 and hyphen.
bool IsUtilityTabKey(std::string_view key);

// Security: a utility tab loads only http/https pages, never an internal or
// file URL the guest could otherwise reach through it.
bool IsUtilityTabUrl(const GURL& url);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_UTILITY_TAB_OPTIONS_H_
