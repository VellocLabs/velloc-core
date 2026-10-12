#ifndef CUSTOM_BROWSER_COMMON_BRANDING_CONSTANTS_H_
#define CUSTOM_BROWSER_COMMON_BRANDING_CONSTANTS_H_

#include <string_view>

namespace custom_browser {

// Single source of truth for the user-visible product name: rebrand by changing
// the two display-name constants below. std::string_view so call sites can pass
// them to base::ASCIIToUTF16().
// Stable brand identifier for filesystem paths: one token without spaces,
// mirroring gn `branding_path_component`. Not changed when rebranding.
inline constexpr std::string_view kCustomBrowserBrandPathComponent = "Velloc";

inline constexpr std::string_view kCustomBrowserProductName = "Velloc Code";
inline constexpr std::string_view kCustomBrowserProductLongName =
    "Velloc Code in Chrome";

// The publisher: Apps & features' "Publisher" column (setup.exe,
// InstallUtil::GetPublisherName) and the copyright holder. Mirrors
// COMPANY_FULLNAME in the brand's BRANDING file, which names the version
// resources; installer_strings_brand_test.py fails when the two drift.
inline constexpr std::string_view kCustomBrowserPublisherName = "VellocLabs";

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_COMMON_BRANDING_CONSTANTS_H_
