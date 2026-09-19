// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/appearance/nexus_appearance_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>

#include "base/values.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/color/chrome_color_mixers.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/testing_profile.h"
#include "components/color/color_mixers.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_registration.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/color/color_provider_manager.h"

namespace nexus {
namespace {

// Records bridge responses so a test can assert how a dispatched action
// settled (mirrors the remote-access adapter fixture).
class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int error_count = 0;
  base::Value last_success;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*result_binary_field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    ++error_count;
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue AppearanceRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "appearance");
  route.Set("action", std::string(action));
  return route;
}

class NexusAppearanceBridgeAdapterTest : public testing::Test {
 protected:
  NexusAppearanceBridgeAdapterTest() {
    // The resolveTheme pipeline reads the global ColorProviderManager, which
    // chrome_browser_main.cc populates with the components + Chrome color
    // mixers at startup. Unit-test binaries never run that, so register the
    // same two initializers here or GetColor() would return defaults.
    ui::ColorProviderManager::Get().AppendColorProviderInitializer(
        base::BindRepeating(color::AddComponentsColorMixers));
    ui::ColorProviderManager::Get().AppendColorProviderInitializer(
        base::BindRepeating(AddChromeColorMixers));
  }

  std::unique_ptr<TestingProfile> CreateProfile() {
    auto prefs =
        std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
    RegisterUserProfilePrefs(prefs->registry());
    RegisterNexusProfilePrefs(prefs->registry());
    TestingProfile::Builder builder;
    builder.SetPrefService(std::move(prefs));
    test::AddGlicDisablingFactory(builder);
    return builder.Build();
  }

  void RunUntilIdle() { task_environment_.RunUntilIdle(); }

  content::BrowserTaskEnvironment task_environment_;
  RecordingDelegate delegate_;
};

// Dispatch one action and let the posted BridgeResponseCallback land.
bool DispatchAndSettle(NexusAppearanceBridgeAdapter& adapter,
                       RecordingDelegate& delegate,
                       content::BrowserTaskEnvironment& env,
                       std::string_view action,
                       base::DictValue payload) {
  const bool dispatched = adapter.Dispatch(std::string(action), "1",
                                           AppearanceRoute(action),
                                           std::move(payload), /*ports=*/{});
  env.RunUntilIdle();
  return dispatched;
}

// resolveTheme resolves a full colour theme for a light scheme WITHOUT
// writing any profile pref: the colours come back as the request's reply, and
// the profile's ThemeService prefs (scheme / user colour / variant) are
// untouched.
TEST_F(NexusAppearanceBridgeAdapterTest,
       ResolveThemeLightResolvesWithoutWritingPrefs) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  PrefService* prefs = profile->GetPrefs();
  const int scheme_before = prefs->GetInteger(prefs::kBrowserColorScheme);
  const int user_color_before = prefs->GetInteger(prefs::kUserColor);
  const int variant_before = prefs->GetInteger(prefs::kBrowserColorVariant);

  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());
  base::DictValue payload;
  payload.Set("scheme", "light");
  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "resolveTheme", std::move(payload)));

  ASSERT_EQ(delegate_.error_count, 0);
  ASSERT_EQ(delegate_.success_count, 1);
  const base::DictValue& result = delegate_.last_success.GetDict();
  EXPECT_EQ(*result.FindString("scheme"), "light");
  const base::DictValue* colors = result.FindDict("colors");
  ASSERT_TRUE(colors);
  // The core roles all resolved to a concrete rgb() string.
  for (const char* role :
       {"surface", "toolbar", "textPrimary", "primary", "onPrimary",
        "focusRing"}) {
    const std::string* value = colors->FindString(role);
    ASSERT_TRUE(value) << role;
    EXPECT_EQ(value->find("rgb("), 0u) << role << " = " << *value;
  }

  // NO pref write: the profile's theme prefs are exactly as before.
  EXPECT_EQ(prefs->GetInteger(prefs::kBrowserColorScheme), scheme_before);
  EXPECT_EQ(prefs->GetInteger(prefs::kUserColor), user_color_before);
  EXPECT_EQ(prefs->GetInteger(prefs::kBrowserColorVariant), variant_before);
}

// A dark scheme resolves too, and differs from light (the surface inverts) —
// proving the scheme actually drives the resolution rather than the profile's
// own palette being echoed.
TEST_F(NexusAppearanceBridgeAdapterTest, ResolveThemeDarkDiffersFromLight) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  base::DictValue light_payload;
  light_payload.Set("scheme", "light");
  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "resolveTheme", std::move(light_payload)));
  const std::string light_surface =
      *delegate_.last_success.GetDict().FindDict("colors")->FindString(
          "surface");

  delegate_.success_count = 0;
  base::DictValue dark_payload;
  dark_payload.Set("scheme", "dark");
  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "resolveTheme", std::move(dark_payload)));
  const base::DictValue& result = delegate_.last_success.GetDict();
  EXPECT_EQ(*result.FindString("scheme"), "dark");
  const std::string dark_surface =
      *result.FindDict("colors")->FindString("surface");

  EXPECT_NE(light_surface, dark_surface);
}

// A seeded colour changes the resolved palette (the accent moves toward the
// seed) without touching the profile — the per-client case.
TEST_F(NexusAppearanceBridgeAdapterTest, ResolveThemeSeedChangesPalette) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  base::DictValue stock;
  stock.Set("scheme", "light");
  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "resolveTheme", std::move(stock)));
  const std::string stock_primary =
      *delegate_.last_success.GetDict().FindDict("colors")->FindString(
          "primary");

  delegate_.success_count = 0;
  base::DictValue seeded;
  seeded.Set("scheme", "light");
  seeded.Set("seed", "#ff0000");
  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "resolveTheme", std::move(seeded)));
  const std::string seeded_primary =
      *delegate_.last_success.GetDict().FindDict("colors")->FindString(
          "primary");

  EXPECT_NE(stock_primary, seeded_primary);
  // And the profile's user-colour pref was NOT set as a side effect.
  EXPECT_FALSE(
      ThemeServiceFactory::GetForProfile(profile.get())->GetUserColor()
          .has_value());
}

// scheme: anything but light|dark is rejected — "system" most of all, since
// the client is required to collapse that against its own device OS.
TEST_F(NexusAppearanceBridgeAdapterTest, ResolveThemeRejectsBadScheme) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  for (const char* bad : {"system", "blue", ""}) {
    base::DictValue payload;
    payload.Set("scheme", bad);
    ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                  "resolveTheme", std::move(payload)))
        << bad;
  }
  EXPECT_EQ(delegate_.success_count, 0);
  EXPECT_EQ(delegate_.error_count, 3);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

// seed must be exactly #rrggbb; the 3- and 8-digit forms and non-hex are
// rejected (the same ParseHexColor the deprecated setUserColor route uses).
TEST_F(NexusAppearanceBridgeAdapterTest, ResolveThemeRejectsBadSeed) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  for (const char* bad_seed : {"#fff", "#ff000080", "red", "ff0000"}) {
    base::DictValue payload;
    payload.Set("scheme", "light");
    payload.Set("seed", bad_seed);
    ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                  "resolveTheme", std::move(payload)))
        << bad_seed;
  }
  EXPECT_EQ(delegate_.success_count, 0);
  EXPECT_EQ(delegate_.error_count, 4);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

// variant must be one of the four named values.
TEST_F(NexusAppearanceBridgeAdapterTest, ResolveThemeRejectsBadVariant) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  base::DictValue payload;
  payload.Set("scheme", "light");
  payload.Set("seed", "#3366ff");
  payload.Set("variant", "psychedelic");
  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "resolveTheme", std::move(payload)));
  EXPECT_EQ(delegate_.success_count, 0);
  EXPECT_EQ(delegate_.error_count, 1);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

// The two write routes are DEPRECATED but must still dispatch for one
// release: the /v1/ guest currently served (nexus_web main) calls them from
// its Settings · Appearance page, so a binary that drops them breaks that
// page. They keep their pre-removal behaviour — writing the profile theme
// pref through ThemeService.
TEST_F(NexusAppearanceBridgeAdapterTest,
       DeprecatedSetColorSchemeDispatchesAndWritesPref) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  PrefService* prefs = profile->GetPrefs();
  const int scheme_before = prefs->GetInteger(prefs::kBrowserColorScheme);

  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());
  base::DictValue payload;
  payload.Set("scheme", "dark");
  EXPECT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "setColorScheme", std::move(payload)));

  ASSERT_EQ(delegate_.error_count, 0);
  ASSERT_EQ(delegate_.success_count, 1);
  EXPECT_TRUE(delegate_.last_success.GetDict().FindBool("ok").value_or(false));

  // The profile pref moved to kDark (and therefore differs from the fresh
  // profile default).
  EXPECT_EQ(prefs->GetInteger(prefs::kBrowserColorScheme),
            static_cast<int>(ThemeService::BrowserColorScheme::kDark));
  EXPECT_NE(prefs->GetInteger(prefs::kBrowserColorScheme), scheme_before);
}

// setColorScheme still validates: anything outside system/light/dark is
// INVALID_ARGS and writes nothing.
TEST_F(NexusAppearanceBridgeAdapterTest, DeprecatedSetColorSchemeRejectsBad) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  base::DictValue payload;
  payload.Set("scheme", "blue");
  EXPECT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "setColorScheme", std::move(payload)));
  EXPECT_EQ(delegate_.success_count, 0);
  EXPECT_EQ(delegate_.error_count, 1);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

// setUserColor writes the seed + variant prefs.
TEST_F(NexusAppearanceBridgeAdapterTest,
       DeprecatedSetUserColorDispatchesAndWritesPref) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  base::DictValue payload;
  payload.Set("color", "#3366ff");
  payload.Set("variant", "vibrant");
  EXPECT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "setUserColor", std::move(payload)));

  ASSERT_EQ(delegate_.error_count, 0);
  ASSERT_EQ(delegate_.success_count, 1);
  ThemeService* theme_service =
      ThemeServiceFactory::GetForProfile(profile.get());
  ASSERT_TRUE(theme_service->GetUserColor().has_value());
  EXPECT_EQ(theme_service->GetBrowserColorVariant(),
            ui::mojom::BrowserColorVariant::kVibrant);

  // Omitting the colour clears back to the default palette.
  delegate_.success_count = 0;
  EXPECT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "setUserColor", base::DictValue()));
  ASSERT_EQ(delegate_.success_count, 1);
  EXPECT_FALSE(theme_service->GetUserColor().has_value());
}

// setUserColor still validates the seed form and the variant name.
TEST_F(NexusAppearanceBridgeAdapterTest, DeprecatedSetUserColorRejectsBad) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  base::DictValue bad_color;
  bad_color.Set("color", "3366ff");  // missing '#'
  EXPECT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "setUserColor", std::move(bad_color)));

  base::DictValue bad_variant;
  bad_variant.Set("color", "#3366ff");
  bad_variant.Set("variant", "psychedelic");
  EXPECT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "setUserColor", std::move(bad_variant)));

  EXPECT_EQ(delegate_.success_count, 0);
  EXPECT_EQ(delegate_.error_count, 2);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

// All five appearance routes dispatch (the three current + the two
// deprecated).
TEST_F(NexusAppearanceBridgeAdapterTest, AllFiveRoutesDispatch) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  EXPECT_TRUE(adapter.Dispatch("getAppearance", "1",
                               AppearanceRoute("getAppearance"),
                               base::DictValue(), {}));
  EXPECT_TRUE(adapter.Dispatch("resolveTheme", "2",
                               AppearanceRoute("resolveTheme"),
                               base::DictValue(), {}));
  base::DictValue scheme_payload;
  scheme_payload.Set("scheme", "light");
  EXPECT_TRUE(adapter.Dispatch("setColorScheme", "3",
                               AppearanceRoute("setColorScheme"),
                               std::move(scheme_payload), {}));
  EXPECT_TRUE(adapter.Dispatch("setUserColor", "4",
                               AppearanceRoute("setUserColor"),
                               base::DictValue(), {}));
  EXPECT_TRUE(adapter.Dispatch("setUiLocale", "5",
                               AppearanceRoute("setUiLocale"),
                               base::DictValue(), {}));
}

// getAppearance reports the locale and, on a fresh profile, the legacy colour
// fields are present-but-default / absent (nothing to migrate): it must NOT
// require scheme/variant/color to be meaningful.
TEST_F(NexusAppearanceBridgeAdapterTest, GetAppearanceLocaleAndLegacyFields) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusAppearanceBridgeAdapter adapter(&delegate_, profile.get());

  ASSERT_TRUE(DispatchAndSettle(adapter, delegate_, task_environment_,
                                "getAppearance", base::DictValue()));
  ASSERT_EQ(delegate_.success_count, 1);
  const base::DictValue& result = delegate_.last_success.GetDict();

  // locale is always present (possibly empty in the test environment).
  EXPECT_TRUE(result.contains("locale"));

  // Legacy fields are OPTIONAL: scheme/variant surface only because the
  // ThemeService exists here; color is absent on a fresh profile (no user
  // colour to migrate). The contract is that a client must not require them.
  EXPECT_TRUE(result.contains("scheme"));
  EXPECT_TRUE(result.contains("variant"));
  EXPECT_FALSE(result.contains("color"));
}

}  // namespace
}  // namespace nexus
