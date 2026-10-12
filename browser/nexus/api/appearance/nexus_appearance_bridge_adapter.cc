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
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/mojom/themes.mojom.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/color/color_provider_manager.h"
#include "ui/native_theme/native_theme.h"

namespace nexus {

namespace {

// The locales shipped on both the browser chrome and the guest web app.
// Security: this list is the enforcement point for the privileged pref write;
// the guest's dropdown is web content and cannot be trusted. Widening support
// means editing this array and the guest's list together. (G-1)
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

// system / light / dark -> the pref enum. Used by the DEPRECATED
// setColorScheme route only; resolveTheme deliberately takes light|dark alone.
// Returns false on anything else so the caller can answer InvalidArgs rather
// than silently picking a default the user did not ask for.
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

// The four variants the guest can name, for the DEPRECATED setUserColor
// route. kSystem is reachable from Chrome's own UI but is not offered here:
// the guest pairs a seed colour with an explicit variant, and kSystem would
// ignore the seed.
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

// --- resolveTheme ------------------------------------------------------------

// light / dark -> the colour-provider key's mode. `system` is NOT accepted —
// the client collapses that against its own device OS before calling, so
// receiving it is a wire bug worth rejecting, not a value to honour.
bool ParseColorMode(std::string_view raw, ui::ColorProviderKey::ColorMode& out) {
  if (raw == "light") {
    out = ui::ColorProviderKey::ColorMode::kLight;
    return true;
  }
  if (raw == "dark") {
    out = ui::ColorProviderKey::ColorMode::kDark;
    return true;
  }
  return false;
}

// The four variants the guest can name, as the colour-provider key's
// SchemeVariant. 1:1 with ui::mojom::BrowserColorVariant (kSystem excluded —
// the guest always pairs a seed with an explicit variant, and kSystem would
// ignore it).
bool ParseSchemeVariant(std::string_view raw,
                        ui::ColorProviderKey::SchemeVariant& out) {
  if (raw == "tonalSpot") {
    out = ui::ColorProviderKey::SchemeVariant::kTonalSpot;
    return true;
  }
  if (raw == "neutral") {
    out = ui::ColorProviderKey::SchemeVariant::kNeutral;
    return true;
  }
  if (raw == "vibrant") {
    out = ui::ColorProviderKey::SchemeVariant::kVibrant;
    return true;
  }
  if (raw == "expressive") {
    out = ui::ColorProviderKey::SchemeVariant::kExpressive;
    return true;
  }
  return false;
}

// The colour roles the guest understands, each resolved from one ui::ColorId.
// This is THE place to tune "which Chrome colour does the guest use for X" on
// the C++ side; it mirrors HOST_THEME_ROLES in
// resources/browser_webui/pages/nexus/nexus_host_theme.ts (the legacy
// broadcast), which reads the SAME palette from chrome://theme/colors.css —
// both are fed by the one ui::ColorProviderManager pipeline, so a role
// resolves to the same colour here as there. The css var each id feeds is
// noted per row.
const struct RoleColorId {
  const char* role;
  ui::ColorId id;
} kRoleColorIds[] = {
    // --color-sys-surface.
    {"surface", ui::kColorSysSurface},
    // --color-toolbar (the toolbar / active-tab surface). Chrome-specific
    // ids live at global scope (chrome_color_id.h), not in namespace ui.
    {"toolbar", ::kColorToolbar},
    // --color-sys-surface1 (raised surfaces).
    {"surfaceRaised", ui::kColorSysSurface1},
    // --color-sys-surface3 (recessed / containers).
    {"surfaceContainer", ui::kColorSysSurface3},
    // The sidebar / composer sit on the page surface (one flat ground).
    {"sidebar", ui::kColorSysSurface},
    {"base", ui::kColorSysSurface},
    // --color-sys-on-surface (body text).
    {"textPrimary", ui::kColorSysOnSurface},
    // --color-secondary-foreground (falls back to on-surface-variant).
    {"textSecondary", ui::kColorSysOnSurfaceVariant},
    // Hairlines: the secondary TEXT colour, mixed to ~20% by the guest.
    {"outline", ui::kColorSysOnSurfaceVariant},
    // --color-sys-primary / --color-sys-on-primary (the accent + text on it).
    {"primary", ui::kColorSysPrimary},
    {"onPrimary", ui::kColorSysOnPrimary},
    // --color-sys-state-focus-ring (focusRing).
    {"focusRing", ui::kColorSysStateFocusRing},
};

// Render a resolved colour the way the guest expects: the same `rgb(r, g, b)`
// string form getComputedStyle returns and HostThemePayload carries. Opaque —
// every role above resolves to an opaque palette colour, so the alpha byte is
// always 0xff and "rgba(...)" would be noise.
std::string RgbStringFromSkColor(SkColor color) {
  return base::StringPrintf("rgb(%u, %u, %u)", SkColorGetR(color),
                            SkColorGetG(color), SkColorGetB(color));
}

// Build the colour-provider key for one resolveTheme request. Mirrors
// NativeTheme::GetColorProviderKey so contrast / system_theme / frame match
// the user's actual environment, then overrides the three request-driven
// fields (mode, user colour, scheme variant). NO profile state is read for
// these three — that is the whole point of per-client resolution.
ui::ColorProviderKey BuildResolveKey(
    ui::ColorProviderKey::ColorMode color_mode,
    std::optional<SkColor> seed,
    std::optional<ui::ColorProviderKey::SchemeVariant> variant) {
  const ui::NativeTheme* native_theme =
      ui::NativeTheme::GetInstanceForNativeUi();
  ui::ColorProviderKey key = native_theme->GetColorProviderKey(nullptr);
  key.color_mode = color_mode;
  // A seed with no explicit variant defaults to tonalSpot (the wire contract);
  // no seed at all means the Chrome stock palette, so both colour fields are
  // cleared (a nullopt user_color ignores scheme_variant).
  key.user_color = seed;
  if (seed.has_value()) {
    key.user_color_source = ui::ColorProviderKey::UserColorSource::kAccent;
    key.scheme_variant =
        variant.value_or(ui::ColorProviderKey::SchemeVariant::kTonalSpot);
  } else {
    key.scheme_variant = std::nullopt;
  }
  return key;
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
  base::DictValue result;

  // The prospective locale (what the next launch uses). `intl.app_locale` is a
  // local-state pref outside ChromeOS: reading it from the profile CHECK-fails.
  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  result.Set("locale",
             local_state ? local_state->GetString(
                               language::prefs::kApplicationLocale)
                         : std::string());

  // The three LEGACY colour fields, kept OPTIONAL for one release so the
  // desktop panel's one-shot migration can read the profile's previous
  // ThemeService values. New clients resolve colours through resolveTheme and
  // never read these. Each is emitted only when the profile actually has a
  // value to migrate (a fresh profile emits none). ThemeService may be absent
  // (no profile); the locale above is still returned then.
  ThemeService* theme_service =
      profile_ ? ThemeServiceFactory::GetForProfile(profile_) : nullptr;
  if (theme_service) {
    result.Set("scheme", std::string(ColorSchemeToString(
                             theme_service->GetBrowserColorScheme())));
    result.Set("variant", std::string(ColorVariantToString(
                              theme_service->GetBrowserColorVariant())));
    // Absent rather than null when unset — the guest reads "no user colour"
    // as "the default swatch is the selected one".
    const std::optional<SkColor> user_color = theme_service->GetUserColor();
    if (user_color.has_value()) {
      result.Set("color", HexColorFromSkColor(user_color.value()));
    }
  }

  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppearanceBridgeAdapter::HandleResolveThemeOnUI(
    ::nexus::generated::ResolveThemeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // scheme is required and must be light|dark — never "system" (the client
  // collapses that against its own device OS; receiving it is a wire bug).
  ui::ColorProviderKey::ColorMode color_mode;
  if (!ParseColorMode(input.scheme, color_mode)) {
    std::move(done).InvalidArgs("scheme must be one of light, dark");
    return;
  }

  // seed: ABSENT = Chrome stock palette; present must be exactly #rrggbb.
  std::optional<SkColor> seed;
  if (input.seed.has_value() && !input.seed->empty()) {
    SkColor parsed = SK_ColorTRANSPARENT;
    if (!ParseHexColor(*input.seed, parsed)) {
      std::move(done).InvalidArgs("seed must be #rrggbb or absent");
      return;
    }
    seed = parsed;
  }

  // variant: ABSENT = tonalSpot (applied in BuildResolveKey); present must be
  // one of the four.
  std::optional<ui::ColorProviderKey::SchemeVariant> variant;
  if (input.variant.has_value()) {
    ui::ColorProviderKey::SchemeVariant parsed =
        ui::ColorProviderKey::SchemeVariant::kTonalSpot;
    if (!ParseSchemeVariant(*input.variant, parsed)) {
      std::move(done).InvalidArgs(
          "variant must be one of tonalSpot, neutral, vibrant, expressive");
      return;
    }
    variant = parsed;
  }

  // Resolve through the chrome://theme/colors.css pipeline keyed by the request;
  // no profile pref is read or written and nothing is broadcast.
  // Security: a remote client at observe scope picks the key and the manager
  // never evicts, so its cache is dropped after kMaxDistinctResolvedThemes keys.
  const std::string resolve_key = base::StringPrintf(
      "%d|%s|%s", static_cast<int>(color_mode),
      seed.has_value() ? base::NumberToString(*seed).c_str() : "",
      input.variant.has_value() ? input.variant->c_str() : "");
  if (!resolved_theme_keys_.contains(resolve_key)) {
    if (resolved_theme_keys_.size() >= kMaxDistinctResolvedThemes) {
      ui::ColorProviderManager::Get().ResetColorProviderCache();
      resolved_theme_keys_.clear();
    }
    resolved_theme_keys_.insert(resolve_key);
  }
  ui::ColorProvider* provider =
      ui::ColorProviderManager::Get().GetColorProviderFor(
          BuildResolveKey(color_mode, seed, variant));
  if (!provider) {
    std::move(done).Error("APPEARANCE_ERROR", "ColorProvider unavailable");
    return;
  }

  base::DictValue colors;
  for (const RoleColorId& role : kRoleColorIds) {
    // Every role here is guaranteed a mixer entry in the Chrome pipeline; if
    // one somehow does not resolve the ColorProvider returns a default and we
    // still emit it (the guest ignores roles it does not know and keeps its
    // own fallback for any role we omit, so omitting buys nothing here).
    colors.Set(role.role, RgbStringFromSkColor(provider->GetColor(role.id)));
  }

  base::DictValue result;
  result.Set("scheme", input.scheme);  // echoed verbatim
  result.Set("colors", std::move(colors));
  std::move(done).Success(base::Value(std::move(result)));
}

// DEPRECATED, one release only — see the api.json and the class comment.
// Behaviour is exactly the pre-removal behaviour: write the profile theme
// pref; the panel's ColorChangeUpdater re-posts nexus-theme and the served
// guest repaints. The served /v1/ guest's Settings · Appearance page still
// calls this, so it must keep working until that guest is replaced.
void NexusAppearanceBridgeAdapter::HandleSetColorSchemeOnUI(
    ::nexus::generated::SetColorSchemeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  ThemeService::BrowserColorScheme scheme;
  if (!ParseColorScheme(input.scheme, scheme)) {
    std::move(done).InvalidArgs("scheme must be one of system, light, dark");
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

// DEPRECATED, one release only — see HandleSetColorSchemeOnUI.
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
