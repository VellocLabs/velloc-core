#ifndef CUSTOM_BROWSER_COMMON_PRODUCT_VERSION_H_
#define CUSTOM_BROWSER_COMMON_PRODUCT_VERSION_H_

#include "custom_browser/common/generated/product_version.inc"

namespace custom_browser {

// Keep this separate from Chromium's version. Composed at build time as
//   {CUSTOM_BROWSER_MAJOR}.{chromium MAJOR}.{bridge interface version}.{CUSTOM_BROWSER_PATCH}
// (e.g. "1.149.1.0") from custom_browser/VERSION, chrome/VERSION, and
// browser/nexus/nexus_interface_version.h — so the app version always names
// the nexus_web /v<N>/ contract this binary speaks. See
// common/scripts/gen_product_version.py and G-3.
inline constexpr char kCustomBrowserProductVersion[] =
    CUSTOM_BROWSER_PRODUCT_VERSION;

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_COMMON_PRODUCT_VERSION_H_
