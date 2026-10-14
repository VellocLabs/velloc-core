// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/launcher/nexus_background_mode_manager.h"

#include "base/functional/callback.h"
#include "chrome/browser/lifetime/termination_notification.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/testing_browser_process.h"
#include "components/keep_alive_registry/keep_alive_registry.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/prefs/testing_pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusBackgroundModeManagerTest : public testing::Test {
 protected:
  // TestingBrowserProcess registers all local-state prefs (including ours and
  // Chrome's background-mode pref, via browser_prefs.cc RegisterLocalState) in
  // its constructor, and exposes that store as both local_state() and
  // GetTestingLocalState(). The pref store is process-global within the test
  // binary, so reset both prefs to "never set" before each test.
  void SetUp() override {
    local_state()->ClearPref(::prefs::kBackgroundModeEnabled);
    local_state()->ClearPref(prefs::kNexusLauncherEnabled);
  }

  TestingPrefServiceSimple* local_state() {
    return TestingBrowserProcess::GetGlobal()->GetTestingLocalState();
  }
  void SetEnabled(bool enabled) {
    local_state()->SetBoolean(::prefs::kBackgroundModeEnabled, enabled);
  }
  bool NexusOriginRegistered() const {
    return KeepAliveRegistry::GetInstance()->IsOriginRegistered(
        KeepAliveOrigin::NEXUS_LAUNCHER);
  }

  content::BrowserTaskEnvironment task_environment_;
};

// The single System toggle is Chrome's prefs::kBackgroundModeEnabled, which
// defaults to TRUE: a fresh install keeps the browser alive after the last
// window closes. Toggling it off releases the keep-alive, on re-acquires it.
// (No status tray in unit tests, so only the keep-alive is exercised here.)
TEST_F(NexusBackgroundModeManagerTest, KeepAliveTracksBackgroundModePref) {
  ASSERT_TRUE(local_state()->GetBoolean(::prefs::kBackgroundModeEnabled));
  ASSERT_FALSE(NexusOriginRegistered());

  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  EXPECT_TRUE(manager.IsKeepingAliveForTesting());
  EXPECT_TRUE(NexusOriginRegistered());

  SetEnabled(false);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());

  SetEnabled(true);
  EXPECT_TRUE(manager.IsKeepingAliveForTesting());
  EXPECT_TRUE(NexusOriginRegistered());
}

// A manager constructed while the pref is off holds nothing.
TEST_F(NexusBackgroundModeManagerTest, NoKeepAliveWhenDisabledAtStartup) {
  SetEnabled(false);

  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());
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

// The legacy nexus.launcher_enabled pref no longer drives anything: a stale
// `true` there does not hold the keep-alive once the new pref is off.
TEST_F(NexusBackgroundModeManagerTest, LegacyLauncherPrefIsNotRead) {
  SetEnabled(false);
  local_state()->SetBoolean(prefs::kNexusLauncherEnabled, true);

  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
}

// A try-to-quit exit (CDP Browser.close, restart, end-session) fires
// APP_TERMINATING once the last browser closes. The manager must drop its
// keep-alive then, or the process outlives every window with only the parked
// Nexus page left. Once terminating, re-enabling the pref must
// not take a new keep-alive either.
//
// The ONLY test that calls NotifyAppTerminating(): it fires at most once per
// process, so a second caller in the same test batch would see a no-op.
TEST_F(NexusBackgroundModeManagerTest, ReleasesKeepAliveOnAppTerminating) {
  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  ASSERT_TRUE(manager.IsKeepingAliveForTesting());

  browser_shutdown::NotifyAppTerminating();
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());

  SetEnabled(false);
  SetEnabled(true);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());
}

// --- Tray "Quit" asks first (remote-access devices) -------------

// Stands in for the UI layer: records what the manager asks for, and lets the
// test answer the confirmation.
class FakeLauncherDelegate : public NexusLauncherDelegate {
 public:
  void OpenOrFocusNexusWindow() override {}
  void OpenOrFocusNexusWindowForProfile(Profile* /*profile*/) override {}
  void OpenOrFocusNormalWindow() override {}
  void ConfirmQuit(base::OnceClosure proceed) override {
    ++confirmations;
    pending = std::move(proceed);
  }
  void Quit() override { ++quits; }

  int confirmations = 0;
  int quits = 0;
  base::OnceClosure pending;
};

class NexusBackgroundModeManagerQuitTest
    : public NexusBackgroundModeManagerTest {
 protected:
  void SetUp() override {
    NexusBackgroundModeManagerTest::SetUp();
    SetNexusLauncherDelegate(&delegate_);
  }
  void TearDown() override { SetNexusLauncherDelegate(nullptr); }

  FakeLauncherDelegate delegate_;
};

// The keep-alive is still held while the user is being asked: dropping it
// first would let the process exit behind the dialog.
TEST_F(NexusBackgroundModeManagerQuitTest, AsksBeforeLettingTheProcessGo) {
  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  ASSERT_TRUE(manager.IsKeepingAliveForTesting());

  manager.Quit();
  EXPECT_EQ(delegate_.confirmations, 1);
  EXPECT_TRUE(manager.IsKeepingAliveForTesting());
  EXPECT_TRUE(NexusOriginRegistered());
  EXPECT_EQ(delegate_.quits, 0);

  std::move(delegate_.pending).Run();
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(NexusOriginRegistered());
  EXPECT_EQ(delegate_.quits, 1);
}

// Declining is dropping the closure: nothing is released, nothing closes.
TEST_F(NexusBackgroundModeManagerQuitTest, ADeclinedQuitChangesNothing) {
  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  manager.Quit();
  delegate_.pending.Reset();
  EXPECT_TRUE(manager.IsKeepingAliveForTesting());
  EXPECT_TRUE(NexusOriginRegistered());
  EXPECT_EQ(delegate_.quits, 0);
}

// A confirmation answered after the manager is gone (shutdown already under
// way) is a no-op, not a use-after-free.
TEST_F(NexusBackgroundModeManagerQuitTest, ALateConfirmationIsHarmless) {
  {
    NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
    manager.Quit();
  }
  std::move(delegate_.pending).Run();
  EXPECT_EQ(delegate_.quits, 0);
}

// --- Migration of the old "Show in the system tray" choice -----------------

// A user who had turned the old tray toggle OFF (explicitly false) keeps it
// off: the new pref (default true) is overwritten with false, and the legacy
// pref is cleared so this only ever happens once.
TEST_F(NexusBackgroundModeManagerTest, MigratesExplicitLegacyOffToNewPref) {
  local_state()->SetBoolean(prefs::kNexusLauncherEnabled, false);
  ASSERT_FALSE(local_state()->HasPrefPath(::prefs::kBackgroundModeEnabled));

  NexusBackgroundModeManager::MigrateLegacyLauncherPref(local_state());

  EXPECT_TRUE(local_state()->HasPrefPath(::prefs::kBackgroundModeEnabled));
  EXPECT_FALSE(local_state()->GetBoolean(::prefs::kBackgroundModeEnabled));
  EXPECT_FALSE(local_state()->HasPrefPath(prefs::kNexusLauncherEnabled));
}

// A user who had the old tray toggle ON gets the new toggle on (a user-set
// true, not just the default), legacy cleared.
TEST_F(NexusBackgroundModeManagerTest, MigratesLegacyOnToNewPref) {
  local_state()->SetBoolean(prefs::kNexusLauncherEnabled, true);

  NexusBackgroundModeManager::MigrateLegacyLauncherPref(local_state());

  EXPECT_TRUE(local_state()->HasPrefPath(::prefs::kBackgroundModeEnabled));
  EXPECT_TRUE(local_state()->GetBoolean(::prefs::kBackgroundModeEnabled));
  EXPECT_FALSE(local_state()->HasPrefPath(prefs::kNexusLauncherEnabled));
}

// A user who never touched the old toggle is untouched: the new pref stays
// at its default and nothing is written.
TEST_F(NexusBackgroundModeManagerTest, NoLegacyValueIsANoOp) {
  NexusBackgroundModeManager::MigrateLegacyLauncherPref(local_state());

  EXPECT_FALSE(local_state()->HasPrefPath(::prefs::kBackgroundModeEnabled));
  EXPECT_TRUE(local_state()->GetBoolean(::prefs::kBackgroundModeEnabled));
}

// The user's NEWER choice on the merged toggle wins over a stale legacy value;
// the legacy pref is still cleared.
TEST_F(NexusBackgroundModeManagerTest, UserSetNewPrefWinsOverLegacy) {
  local_state()->SetBoolean(prefs::kNexusLauncherEnabled, true);
  local_state()->SetBoolean(::prefs::kBackgroundModeEnabled, false);

  NexusBackgroundModeManager::MigrateLegacyLauncherPref(local_state());

  EXPECT_FALSE(local_state()->GetBoolean(::prefs::kBackgroundModeEnabled));
  EXPECT_FALSE(local_state()->HasPrefPath(prefs::kNexusLauncherEnabled));
}

// The manager runs the migration on construction, so a migrated "off" user
// starts without the keep-alive even though the new pref defaults to on.
TEST_F(NexusBackgroundModeManagerTest, ConstructorAppliesMigration) {
  local_state()->SetBoolean(prefs::kNexusLauncherEnabled, false);

  NexusBackgroundModeManager manager(/*status_tray=*/nullptr);
  EXPECT_FALSE(manager.IsKeepingAliveForTesting());
  EXPECT_FALSE(local_state()->HasPrefPath(prefs::kNexusLauncherEnabled));
}

}  // namespace
}  // namespace nexus
