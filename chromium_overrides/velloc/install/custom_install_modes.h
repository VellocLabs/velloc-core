#ifndef CUSTOM_BROWSER_INSTALL_CUSTOM_INSTALL_MODES_H_
#define CUSTOM_BROWSER_INSTALL_CUSTOM_INSTALL_MODES_H_

#include <array>

#include "chrome/app/chrome_dll_resource.h"
#include "chrome/common/chrome_icon_resources_win.h"
#include "chrome/install_static/buildflags.h"
#include "chrome/install_static/install_constants.h"

namespace install_static {

// The brand-specific company name to be included as a component of the install
// and user data directory paths. May be empty if no such dir is to be used.
inline constexpr wchar_t kCompanyPathName[] = L"";

// The brand-specific product name to be included as a component of the install
// and user data directory paths.
inline constexpr wchar_t kProductPathName[] =
    BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT_WIDE);

// The brand-specific safe browsing client name.
inline constexpr char kSafeBrowsingName[] =
    BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT);

// Note: This list of indices must be kept in sync with the brand-specific
// resource strings in chrome/installer/util/prebuild/create_string_rc.
enum InstallConstantIndex {
  CHROMIUM_INDEX,
  NUM_INSTALL_MODES,
};

inline constexpr auto kInstallModes = std::to_array<InstallConstants>({
    // The primary (and only) install mode for the custom browser.
    {
        .size = sizeof(InstallConstants),
        .index = CHROMIUM_INDEX,  // The one and only mode for this brand.
        .install_switch =
            "",  // No install switch for the primary install mode.
        .install_suffix =
            L"",  // Empty install_suffix for the primary install mode.
        .logo_suffix = L"",  // No logo suffix for the primary install mode.
        .app_guid =
            L"{B9363225-2877-4D37-937B-3F761784F75F}",  // Velloc app GUID.
        .base_app_name =
            BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT_WIDE),  // App name.
        .base_app_id =
            BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT_WIDE),  // App ID.
        .browser_prog_id_prefix =
            BUILDFLAG(CUSTOM_BRANDING_BROWSER_PROG_ID_PREFIX_WIDE),
        .browser_prog_id_description =
            BUILDFLAG(CUSTOM_BRANDING_HTML_DOC_DESC_WIDE),
        .direct_launch_url_scheme =
            BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT),
        .pdf_prog_id_prefix =
            BUILDFLAG(CUSTOM_BRANDING_PDF_PROG_ID_PREFIX_WIDE),
        .pdf_prog_id_description =
            BUILDFLAG(CUSTOM_BRANDING_PDF_DOC_DESC_WIDE),
        .active_setup_guid =
            L"{3F419C09-15B6-46BE-8022-345F02FCDBC3}",  // Active Setup GUID.
        .legacy_command_execute_clsid =
            L"{C5B2F711-10BE-4F01-91D8-73F2667FAF7C}",  // CommandExecuteImpl.
        .toast_activator_clsid = {0x7178D34C,
                                  0xD527,
                                  0x4C43,
                                  {0xA7, 0x01, 0x72, 0xD0, 0x78, 0x19, 0x19,
                                   0xF2}},  // Toast Activator CLSID.
        .elevator_clsid = {0xA086AE9D,
                           0xAAA1,
                           0x4623,
                           {0xAD, 0x2A, 0x4D, 0x5B, 0x6C, 0xC6, 0xE1,
                            0xFE}},  // Elevator CLSID.
        .elevator_iid = {0xFF8CA665,
                         0x7E1C,
                         0x4050,
                         {0x8F, 0xCD, 0x81, 0x33, 0xF4, 0x60, 0xC3,
                          0x8F}},  // IElevator IID and TypeLib.
        .tracing_service_clsid = {0xB1DCAB96,
                                  0xE4E3,
                                  0x449F,
                                  {0xB0, 0x29, 0x9A, 0x12, 0xCB, 0x69, 0xFD,
                                   0x0C}},  // SystemTraceSession CLSID.
        .tracing_service_iid = {0xCE3A68BA,
                                0x76ED,
                                0x4AC9,
                                {0x96, 0x35, 0xD7, 0xC3, 0xB3, 0x37, 0x07,
                                 0x30}},  // ISystemTraceSession IID.
        .default_channel_name =
            L"",  // Empty default channel name since no update integration.
        .channel_strategy = ChannelStrategy::UNSUPPORTED,
        .supports_system_level = true,  // Supports system-level installs.
        .supports_set_as_default_browser =
            true,  // Supports in-product set as default browser UX.
        .app_icon_resource_index =
            icon_resources::kApplicationIndex,  // App icon resource index.
        .app_icon_resource_id = IDR_MAINFRAME,  // App icon resource id.
        .html_doc_icon_resource_index =
            icon_resources::kApplicationIndex,  // Use app icon for HTML docs.
        .pdf_doc_icon_resource_index =
            icon_resources::kApplicationIndex,  // Use app icon for PDFs.
        .sandbox_sid_prefix =
            L"S-1-15-2-3251537155-1984446955-2931258699-841473695-"
            L"1938553385-"
            L"924012148-",  // App container sid prefix for sandbox.
    },
});

}  // namespace install_static

#endif  // CUSTOM_BROWSER_INSTALL_CUSTOM_INSTALL_MODES_H_
