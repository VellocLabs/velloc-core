// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"

#include <string>
#include <vector>

#include "components/prefs/testing_pref_service.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

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

// A window whose page never pushes layouts must not erase them on close.
//
// That window is the normal case for the background panel (P-6): the next
// kNexus window adopts a LIVE page, and a page only re-measures when its
// holders resize — a same-size window sends no UpsertNativeViewLayouts at all.
// SaveCurrentUILayoutParams() persists only "touched" entries, so without the
// load-time seed that close wrote an empty set and every stored native-view
// layout for the mode was lost, stripping more of the window's native views
// (the caption buttons among them) on each park/reopen cycle.
TEST_F(CustomBrowserUiPrefsLayoutTest, SilentWindowDoesNotEraseStoredLayouts) {
  RunWindow(/*page_pushes_layouts=*/true);

  // Three park/reopen cycles in which the page stays silent.
  for (int cycle = 0; cycle < 3; ++cycle) {
    const std::vector<std::string> restored =
        RunWindow(/*page_pushes_layouts=*/false);
    EXPECT_EQ(2u, restored.size()) << "layouts lost after cycle " << cycle;
  }
}

}  // namespace
