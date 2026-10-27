// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"

#include <string>
#include <vector>

#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/prefs/testing_pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_web_ui_data_source.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/common/pref_names.h"
#include "custom_browser/common/pref_names.h"
#include "custom_browser/common/url_constants.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "net/base/url_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace {

using custom_browser::mojom::NativeViewParams;
using custom_browser::mojom::NativeViewParamsPtr;
using custom_browser::mojom::UIMode;

// Counts the layouts a window is handed when it restores from prefs — the
// observer callback NativeUIContainerView uses to build its native views.
class LayoutCountingObserver : public CustomBrowserUiPrefs::Observer {
 public:
  void OnLayoutAdded(
      const custom_browser::mojom::NativeViewParams* layout) override {
    if (layout && layout->view_id) {
      added.push_back(*layout->view_id);
    }
  }

  std::vector<std::string> added;
};

NativeViewParamsPtr MakeLayout(int32_t hash_id,
                               const std::string& view_id,
                               bool visible) {
  auto layout = NativeViewParams::New();
  layout->hash_id = hash_id;
  layout->view_id = view_id;
  layout->visible = visible;
  layout->params = custom_browser::mojom::LayoutParams::New();
  layout->params->left = 0;
  layout->params->top = 0;
  layout->params->width = 120;
  layout->params->height = 32;
  return layout;
}

class CustomBrowserUiPrefsLayoutTest : public testing::Test {
 protected:
  void SetUp() override {
    CustomBrowserUiPrefs::RegisterProfilePrefs(prefs_.registry());
  }

  // One window's whole life: restore this mode's stored layouts, optionally
  // receive a page push, then close. Returns the layouts the RESTORE handed
  // out (the push adds its own, which says nothing about what was stored).
  std::vector<std::string> RunWindow(bool page_pushes_layouts) {
    CustomBrowserUiPrefs ui_prefs(&prefs_);
    LayoutCountingObserver observer;
    ui_prefs.AddObserver(&observer);
    ui_prefs.set_initial_mode_override(UIMode::kNexus);
    ui_prefs.RestoreFromPrefs();
    const std::vector<std::string> restored = observer.added;

    if (page_pushes_layouts) {
      std::vector<NativeViewParamsPtr> batch;
      batch.push_back(MakeLayout(/*hash_id=*/1, "caption", /*visible=*/true));
      batch.push_back(MakeLayout(/*hash_id=*/2, "contents", /*visible=*/true));
      ui_prefs.UpdateLayouts(std::move(batch));
    }

    ui_prefs.OnBrowserClosing();
    ui_prefs.RemoveObserver(&observer);
    return restored;
  }

  TestingPrefServiceSimple prefs_;
};

// The first window's page pushes its layouts, and they are persisted for the
// next window to restore.
TEST_F(CustomBrowserUiPrefsLayoutTest, PushedLayoutsArePersisted) {
  // Nothing stored yet, so the first window restores nothing.
  EXPECT_TRUE(RunWindow(/*page_pushes_layouts=*/true).empty());

  const std::vector<std::string> restored =
      RunWindow(/*page_pushes_layouts=*/false);
  EXPECT_EQ(2u, restored.size());
}

// A window whose page never pushes layouts must not erase the stored layouts
// on close: a parked page re-adopted by a same-size window pushes none (P-6).
TEST_F(CustomBrowserUiPrefsLayoutTest, SilentWindowDoesNotEraseStoredLayouts) {
  RunWindow(/*page_pushes_layouts=*/true);

  // Three park/reopen cycles in which the page stays silent.
  for (int cycle = 0; cycle < 3; ++cycle) {
    const std::vector<std::string> restored =
        RunWindow(/*page_pushes_layouts=*/false);
    EXPECT_EQ(2u, restored.size()) << "layouts lost after cycle " << cycle;
  }
}

// Velloc's default replaces upstream's, so startup opens the New Tab page on
// a profile whose first run never applied the installer's initial prefs.
TEST(CustomBrowserUiPrefsDefaultsTest, StartupDefaultsToTheNewTabPage) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterIntegerPref(prefs::kRestoreOnStartup,
                                        SessionStartupPref::kPrefValueLast);
  CustomBrowserUiPrefs::SetVellocProfilePrefDefaults(prefs.registry());

  EXPECT_EQ(SessionStartupPref::kPrefValueNewTab,
            prefs.GetInteger(prefs::kRestoreOnStartup));
  EXPECT_TRUE(prefs.FindPreference(prefs::kRestoreOnStartup)->IsDefaultValue());
  // A value the user chose still wins.
  prefs.SetInteger(prefs::kRestoreOnStartup, SessionStartupPref::kPrefValueLast);
  EXPECT_EQ(SessionStartupPref::kPrefValueLast,
            prefs.GetInteger(prefs::kRestoreOnStartup));
}

// The per-window modes are never read back as the shared current mode (F-29).
TEST(CustomBrowserUiPrefsModeTest, PerWindowModesClampToABrowserMode) {
  TestingPrefServiceSimple prefs;
  CustomBrowserUiPrefs::RegisterProfilePrefs(prefs.registry());

  EXPECT_TRUE(CustomBrowserUiPrefs::IsPerWindowMode(UIMode::kNexus));
  EXPECT_TRUE(CustomBrowserUiPrefs::IsPerWindowMode(UIMode::kSingleWeb));
  EXPECT_FALSE(CustomBrowserUiPrefs::IsPerWindowMode(UIMode::kSidebar));
  EXPECT_FALSE(CustomBrowserUiPrefs::IsPerWindowMode(UIMode::kStandard));

  prefs.SetInteger(kCustomUIModePrefPath, static_cast<int>(UIMode::kSingleWeb));
  EXPECT_EQ(UIMode::kSidebar, CustomBrowserUiPrefs::GetUIModeFromPref(&prefs));
  prefs.SetInteger(kCustomUIModePrefPath,
                   static_cast<int>(UIMode::kCompactStandard));
  EXPECT_EQ(UIMode::kCompactStandard,
            CustomBrowserUiPrefs::GetUIModeFromPref(&prefs));
}

// A page's URL carries its window's own mode — per-window or shared — so
// the page's first layout is that mode and not the profile-global baked one.
TEST(CustomBrowserUiPrefsModeTest, InitUrlCarriesTheWindowMode) {
  TestingPrefServiceSimple prefs;
  CustomBrowserUiPrefs::RegisterProfilePrefs(prefs.registry());
  prefs.SetInteger(kCustomUIModePrefPath,
                   static_cast<int>(UIMode::kCompactStandard));

  const auto mode_on = [](const GURL& url) {
    std::string value;
    return net::GetValueForKeyInQuery(url, "kUIMode", &value) ? value
                                                               : "(none)";
  };
  const auto number = [](UIMode mode) {
    return base::NumberToString(static_cast<int>(mode));
  };

  CustomBrowserUiPrefs shared(&prefs);
  const GURL shared_url = shared.BuildInitUrl();
  EXPECT_EQ(custom_browser::kChromeUICustomBrowserHost, shared_url.host());
  EXPECT_EQ("/", shared_url.path());
  EXPECT_EQ(number(UIMode::kCompactStandard), mode_on(shared_url));

  CustomBrowserUiPrefs nexus(&prefs);
  nexus.set_initial_mode_override(UIMode::kNexus);
  EXPECT_EQ(number(UIMode::kNexus), mode_on(nexus.BuildInitUrl()));
}

// The baked starting mode of a page is the clamped browser mode, never a
// per-window one the dict may hold (F-29): a browser window's page must not
// draw the Nexus panel while it waits for its own window's mode.
class CustomBrowserUiPrefsBakedModeTest : public testing::Test {
 protected:
  std::optional<int> BakedMode(PrefService* prefs) {
    auto source = content::TestWebUIDataSource::Create("custom-browser-test");
    CustomBrowserUiPrefs::InitMainUI(source->GetWebUIDataSource(), prefs);
    return source->GetLocalizedStrings().FindInt("kUIMode");
  }

  content::BrowserTaskEnvironment task_environment_;
};

TEST_F(CustomBrowserUiPrefsBakedModeTest, PerWindowModeBakesAsABrowserMode) {
  TestingPrefServiceSimple prefs;
  CustomBrowserUiPrefs::RegisterProfilePrefs(prefs.registry());

  // A nested store keeps the mode inside the dict, where the global loop
  // bakes it raw: the clamp must win over that entry too.
  prefs.SetInteger(kCustomUIModePrefPath, static_cast<int>(UIMode::kNexus));
  {
    ScopedDictPrefUpdate update(&prefs, kCustomBrowserPrefPath);
    update->Set("kUIMode", static_cast<int>(UIMode::kNexus));
  }
  EXPECT_EQ(static_cast<int>(UIMode::kSidebar), BakedMode(&prefs));

  prefs.SetInteger(kCustomUIModePrefPath,
                   static_cast<int>(UIMode::kCompactStandard));
  {
    ScopedDictPrefUpdate update(&prefs, kCustomBrowserPrefPath);
    update->Set("kUIMode", static_cast<int>(UIMode::kCompactStandard));
  }
  EXPECT_EQ(static_cast<int>(UIMode::kCompactStandard), BakedMode(&prefs));
}

}  // namespace
