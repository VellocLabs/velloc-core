#ifndef CUSTOM_BROWSER_COMMON_PRODUCT_VERSION_H_
#define CUSTOM_BROWSER_COMMON_PRODUCT_VERSION_H_

#include "custom_browser/common/generated/product_version.inc"

namespace custom_browser {

// Keep this separate from Chromium's version. Source of truth is
// custom_browser/VERSION, with MAJOR overridden to match Chromium's MAJOR.
inline constexpr char kCustomBrowserProductVersion[] =
    CUSTOM_BROWSER_PRODUCT_VERSION;

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_COMMON_PRODUCT_VERSION_H_
