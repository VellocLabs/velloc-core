// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/launcher/nexus_background_mode_manager.h"

#include "chrome/test/base/testing_browser_process.h"
#include "components/keep_alive_registry/keep_alive_registry.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/prefs/testing_pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusBackgroundModeManagerTest : public testing::Test {
 protected:
  // TestingBrowserProcess registers all local-state prefs (including ours, via
  // browser_prefs.cc RegisterLocalState under ENABLE_CUSTOM_BROWSER) in its
  // constructor, and exposes that store as both local_state() and
  // GetTestingLocalState(). The pref store is process-global within the test
  // binary, so reset our pref before each test.
  void SetUp() override { SetEnabled(false); }

  TestingPrefServiceSimple* local_state() {
    return TestingBrowserProcess::GetGlobal()->GetTestingLocalState();
  }
  void SetEnabled(bool enabled) {
    local_state()->SetBoolean(prefs::kNexusLauncherEnabled, enabled);
  }
  bool NexusOriginRegistered() const {
    return KeepAliveRegistry::GetInstance()->IsOriginRegistered(
        KeepAliveOrigin::NEXUS_LAUNCHER);
  }

  content::BrowserTaskEnvironment task_environment_;
};

// The pref defaults to false, so a freshly constructed manager holds no
// keep-alive; toggling the pref on acquires it and toggling off releases it.
// This is the core stage-1 guarantee: the browser only survives window-close
// when nexus.launcher_enabled is true. (No status tray in unit tests, so only
// the keep-alive is exercised here.)
TEST_F(NexusBackgroundModeManagerTest, KeepAliveTracksPref) {
  ASSERT_FALSE(local_state()->GetBoolean(prefs::kNexusLauncherEnabled));
  ASSERT_FALSE(NexusOriginRegistered());

  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());

  SetEnabled(true);
  EXPECT_TRUE(manager.IsKeepingAliveForTesting());
  EXPECT_TRUE(NexusOriginRegistered());

  SetEnabled(false);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());
}

// A manager constructed while the pref is already enabled (e.g. the user had
// the launcher on across restarts) acquires the keep-alive immediately.
TEST_F(NexusBackgroundModeManagerTest, HoldsKeepAliveWhenEnabledAtStartup) {
  SetEnabled(true);

  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  EXPECT_TRUE(manager.IsKeepingAliveForTesting());
  EXPECT_TRUE(NexusOriginRegistered());
}

// Releasing the manager must release the keep-alive (no leak across the
// process-global KeepAliveRegistry).
TEST_F(NexusBackgroundModeManagerTest, ReleasesKeepAliveOnDestruction) {
  SetEnabled(true);
  {
    NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
    ASSERT_TRUE(NexusOriginRegistered());
  }
  EXPECT_FALSE(NexusOriginRegistered());
}

}  // namespace
}  // namespace nexus
