#include "custom_browser/browser/nexus/api/appearance/nexus_appearance_bridge_adapter.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/mojom/themes.mojom.h"

namespace nexus {

namespace {

// The locales Velloc ships on BOTH surfaces — the browser chrome
// (custom_browser_strings_<lang>.pak) AND the guest web app
// (nexus_web .../i18n/messages/<locale>.ts). The browser chrome alone also
// carries bn / hi / pt / ru, but selecting one of those would switch the menus
// and leave the web app in English, so they are deliberately not offered.
//
// This list is the ENFORCEMENT point. The guest builds the same dropdown, but
// the guest is web content and cannot be trusted to constrain a privileged
// pref write. Widening the product's language support means editing this array
// and the guest's list together.
// See internal/docs/G-guide/G-1-workflow/translation.md § Supported languages.
constexpr std::string_view kSupportedUiLocales[] = {
    "en", "zh-CN", "es", "fr", "de", "ja", "ko",
};

bool IsSupportedUiLocale(std::string_view locale) {
  for (std::string_view supported : kSupportedUiLocales) {
    if (locale == supported) {
      return true;
    }
  }
  return false;
}

// system / light / dark -> the pref enum. Returns false on anything else so
// the caller can answer InvalidArgs rather than silently picking a default the
// user did not ask for.
bool ParseColorScheme(std::string_view raw,
                      ThemeService::BrowserColorScheme& out) {
  if (raw == "system") {
    out = ThemeService::BrowserColorScheme::kSystem;
    return true;
  }
  if (raw == "light") {
    out = ThemeService::BrowserColorScheme::kLight;
    return true;
  }
  if (raw == "dark") {
    out = ThemeService::BrowserColorScheme::kDark;
    return true;
  }
  return false;
}

std::string_view ColorSchemeToString(ThemeService::BrowserColorScheme scheme) {
  switch (scheme) {
    case ThemeService::BrowserColorScheme::kLight:
      return "light";
    case ThemeService::BrowserColorScheme::kDark:
      return "dark";
    case ThemeService::BrowserColorScheme::kSystem:
      return "system";
  }
  return "system";
}

// The four variants the guest can name. kSystem is reachable from Chrome's own
// UI but is not offered here: the guest pairs a seed colour with an explicit
// variant, and kSystem would ignore the seed.
bool ParseColorVariant(std::string_view raw,
                       ui::mojom::BrowserColorVariant& out) {
  if (raw == "tonalSpot") {
    out = ui::mojom::BrowserColorVariant::kTonalSpot;
    return true;
  }
  if (raw == "neutral") {
    out = ui::mojom::BrowserColorVariant::kNeutral;
    return true;
  }
  if (raw == "vibrant") {
    out = ui::mojom::BrowserColorVariant::kVibrant;
    return true;
  }
  if (raw == "expressive") {
    out = ui::mojom::BrowserColorVariant::kExpressive;
    return true;
  }
  return false;
}

std::string_view ColorVariantToString(ui::mojom::BrowserColorVariant variant) {
  switch (variant) {
    case ui::mojom::BrowserColorVariant::kTonalSpot:
      return "tonalSpot";
    case ui::mojom::BrowserColorVariant::kNeutral:
      return "neutral";
    case ui::mojom::BrowserColorVariant::kVibrant:
      return "vibrant";
    case ui::mojom::BrowserColorVariant::kExpressive:
      return "expressive";
    case ui::mojom::BrowserColorVariant::kSystem:
      return "system";
  }
  return "tonalSpot";
}

// #rrggbb -> opaque SkColor. Strict on purpose: the guest sends one of a fixed
// palette, so anything else is a bug worth surfacing rather than rounding off.
// Rejects the 3-digit and 8-digit forms too — neither is produced by the
// guest, and accepting them would make the wire contract wider than the thing
// that has to honour it.
bool ParseHexColor(std::string_view raw, SkColor& out) {
  if (raw.size() != 7 || raw[0] != '#') {
    return false;
  }
  uint32_t rgb = 0;
  if (!base::HexStringToUInt(raw.substr(1), &rgb)) {
    return false;
  }
  out = SkColorSetRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
  return true;
}

std::string HexColorFromSkColor(SkColor color) {
  return base::StringPrintf("#%02x%02x%02x", SkColorGetR(color),
                            SkColorGetG(color), SkColorGetB(color));
}

}  // namespace

NexusAppearanceBridgeAdapter::NexusAppearanceBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::AppearanceApiBase(delegate), profile_(profile) {}

NexusAppearanceBridgeAdapter::~NexusAppearanceBridgeAdapter() = default;

void NexusAppearanceBridgeAdapter::HandleGetAppearanceOnUI(
    ::nexus::generated::GetAppearanceInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  ThemeService* theme_service =
      profile_ ? ThemeServiceFactory::GetForProfile(profile_) : nullptr;
  if (!theme_service) {
    std::move(done).Error("APPEARANCE_ERROR", "ThemeService unavailable");
    return;
  }

  base::DictValue result;
  result.Set("scheme", std::string(ColorSchemeToString(
                           theme_service->GetBrowserColorScheme())));
  result.Set("variant", std::string(ColorVariantToString(
                            theme_service->GetBrowserColorVariant())));

  // Absent rather than null when unset — the guest reads "no user colour" as
  // "the default swatch is the selected one".
  const std::optional<SkColor> user_color = theme_service->GetUserColor();
  if (user_color.has_value()) {
    result.Set("color", HexColorFromSkColor(user_color.value()));
  }

  // The PROSPECTIVE locale: what the next launch will use, which is what the
  // dropdown must show after a change and before the restart.
  //
  // It lives in LOCAL STATE, not the profile. `intl.app_locale` is registered
  // on the local-state registry (browser_process_impl.cc), and outside
  // ChromeOS it is registered NOWHERE on a profile's registry — so reading it
  // from `profile_->GetPrefs()` is not a wrong value but a CHECK failure in
  // PrefService ("Trying to access an unregistered pref"), which takes the
  // browser process down. Chrome's own languages_handler.cc reads the profile
  // only under BUILDFLAG(IS_CHROMEOS) and falls back to local state; this is
  // the desktop half of that.
  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  result.Set("locale",
             local_state ? local_state->GetString(
                               language::prefs::kApplicationLocale)
                         : std::string());

  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppearanceBridgeAdapter::HandleSetColorSchemeOnUI(
    ::nexus::generated::SetColorSchemeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  ThemeService::BrowserColorScheme scheme;
  if (!ParseColorScheme(input.scheme, scheme)) {
    std::move(done).InvalidArgs(
        "scheme must be one of system, light, dark");
    return;
  }

  ThemeService* theme_service =
      profile_ ? ThemeServiceFactory::GetForProfile(profile_) : nullptr;
  if (!theme_service) {
    std::move(done).Error("APPEARANCE_ERROR", "ThemeService unavailable");
    return;
  }

  // ThemeService notifies its observers, ColorChangeUpdater fires in the
  // panel, nexus_host_theme.ts re-resolves and re-posts. No reload needed.
  theme_service->SetBrowserColorScheme(scheme);

  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppearanceBridgeAdapter::HandleSetUserColorOnUI(
    ::nexus::generated::SetUserColorInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  ThemeService* theme_service =
      profile_ ? ThemeServiceFactory::GetForProfile(profile_) : nullptr;
  if (!theme_service) {
    std::move(done).Error("APPEARANCE_ERROR", "ThemeService unavailable");
    return;
  }

  // Absent or empty clears the user colour back to the default palette.
  // SetUserColorAndBrowserColorVariant takes a bare SkColor and cannot express
  // "none", so clearing goes through SetUserColor(std::nullopt) instead.
  const std::string color_raw = input.color.value_or(std::string());
  if (color_raw.empty()) {
    theme_service->SetUserColor(std::nullopt);
    base::DictValue cleared;
    cleared.Set("ok", true);
    std::move(done).Success(base::Value(std::move(cleared)));
    return;
  }

  SkColor color = SK_ColorTRANSPARENT;
  if (!ParseHexColor(color_raw, color)) {
    std::move(done).InvalidArgs("color must be #rrggbb or absent");
    return;
  }

  ui::mojom::BrowserColorVariant variant =
      ui::mojom::BrowserColorVariant::kTonalSpot;
  if (input.variant.has_value() &&
      !ParseColorVariant(input.variant.value(), variant)) {
    std::move(done).InvalidArgs(
        "variant must be one of tonalSpot, neutral, vibrant, expressive");
    return;
  }

  // Both prefs in one call so the theme update propagates once.
  theme_service->SetUserColorAndBrowserColorVariant(color, variant);

  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppearanceBridgeAdapter::HandleSetUiLocaleOnUI(
    ::nexus::generated::SetUiLocaleInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!IsSupportedUiLocale(input.locale)) {
    std::move(done).InvalidArgs(
        "locale is not one of the languages this build ships");
    return;
  }

  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  if (!local_state) {
    std::move(done).Error("APPEARANCE_ERROR", "local state unavailable");
    return;
  }

  // The same single write chrome://settings/languages performs on Windows
  // (languages_handler.cc HandleSetProspectiveUILanguage). Local state, not
  // the profile — see HandleGetAppearanceOnUI. Read at startup, so it takes
  // effect on the next launch; the guest drives the restart itself through
  // appInfo.relaunch.
  local_state->SetString(language::prefs::kApplicationLocale, input.locale);

  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

}  // namespace nexus
