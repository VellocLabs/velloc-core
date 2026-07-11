#ifndef CUSTOM_BROWSER_COMMON_BRANDING_CONSTANTS_H_
#define CUSTOM_BROWSER_COMMON_BRANDING_CONSTANTS_H_

#include <string_view>

namespace custom_browser {

// Single source of truth for the user-visible product name. To rebrand the
// product (e.g. "Nexus" -> "abc"), change these two constants and rebuild —
// no other source file should hold the visible name as a literal.
//
// Short form is used wherever the source string previously said the bare
// product name. Long form is used wherever the source string previously
// said "<name> in Chrome".
//
// Flow into the WebUI guest: ui/web_ui/custom_browser_webui.cc exposes
// these via loadTimeData as `customBrowserProductName` /
// `customBrowserProductLongName`, and resources/browser_webui/pages/nexus/
// nexus_runtime_bootstrap.ts forwards the short name to the nexus_web
// guest via the `?product=` URL query parameter.
//
// Stored as std::string_view (not char[]) so call sites can pass the
// constants directly to base::ASCIIToUTF16(), which has a static_assert
// blocking the char[N] literal overload (it wants u"..." literals there).
inline constexpr std::string_view kCustomBrowserProductName = "Velloc";
inline constexpr std::string_view kCustomBrowserProductLongName =
    "Velloc in Chrome";

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_COMMON_BRANDING_CONSTANTS_H_
