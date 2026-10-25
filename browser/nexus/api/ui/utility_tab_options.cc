// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/ui/utility_tab_options.h"

#include "url/gurl.h"

namespace nexus {

namespace {

constexpr size_t kUtilityTabKeyMaxLength = 32;

bool IsKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

}  // namespace

bool IsUtilityTabKey(std::string_view key) {
  if (key.empty() || key.size() > kUtilityTabKeyMaxLength) {
    return false;
  }
  for (char c : key) {
    if (!IsKeyChar(c)) {
      return false;
    }
  }
  return true;
}

bool IsUtilityTabUrl(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS();
}

}  // namespace nexus
