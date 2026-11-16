#include "custom_browser/browser/nexus/nexus_guest_menu_model.h"

#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/context_menu_data/edit_flags.h"

namespace nexus {
namespace {

using Cmd = GuestMenuCommand;

constexpr int kAllEditFlags = blink::kCanUndo | blink::kCanRedo |
                              blink::kCanCut | blink::kCanCopy |
                              blink::kCanPaste | blink::kCanSelectAll;

TEST(NexusGuestMenuModelTest, NonEditableWithoutInspectShowsNoMenu) {
  EXPECT_TRUE(BuildGuestMenuItems(/*is_editable=*/false, kAllEditFlags,
                                  /*inspect_enabled=*/false)
                  .empty());
}

TEST(NexusGuestMenuModelTest, NonEditableWithInspectShowsOnlyInspect) {
  const std::vector<GuestMenuItem> expected = {{Cmd::kInspect, true}};
  EXPECT_EQ(BuildGuestMenuItems(/*is_editable=*/false, kAllEditFlags,
                                /*inspect_enabled=*/true),
            expected);
}

TEST(NexusGuestMenuModelTest, EditableShowsGroupedEditCommands) {
  const std::vector<GuestMenuItem> expected = {
      {Cmd::kUndo, true},      {Cmd::kRedo, true},
      {Cmd::kSeparator, true}, {Cmd::kCut, true},
      {Cmd::kCopy, true},      {Cmd::kPaste, true},
      {Cmd::kSeparator, true}, {Cmd::kSelectAll, true},
  };
  EXPECT_EQ(BuildGuestMenuItems(/*is_editable=*/true, kAllEditFlags,
                                /*inspect_enabled=*/false),
            expected);
}

TEST(NexusGuestMenuModelTest, EditFlagsDriveEnabledState) {
  // A caret with no selection: nothing to cut or copy.
  const int flags = blink::kCanUndo | blink::kCanRedo | blink::kCanPaste |
                    blink::kCanSelectAll;
  const std::vector<GuestMenuItem> expected = {
      {Cmd::kUndo, true},      {Cmd::kRedo, true},
      {Cmd::kSeparator, true}, {Cmd::kCut, false},
      {Cmd::kCopy, false},     {Cmd::kPaste, true},
      {Cmd::kSeparator, true}, {Cmd::kSelectAll, true},
  };
  EXPECT_EQ(BuildGuestMenuItems(/*is_editable=*/true, flags,
                                /*inspect_enabled=*/false),
            expected);
}

TEST(NexusGuestMenuModelTest, NoEditFlagsKeepsItemsButDisablesThem) {
  const std::vector<GuestMenuItem> items =
      BuildGuestMenuItems(/*is_editable=*/true, blink::kCanDoNone,
                          /*inspect_enabled=*/false);
  ASSERT_EQ(items.size(), 8u);
  for (const GuestMenuItem& item : items) {
    if (item.command != Cmd::kSeparator) {
      EXPECT_FALSE(item.enabled) << static_cast<int>(item.command);
    }
  }
}

TEST(NexusGuestMenuModelTest, EditableWithInspectAppendsInspectAfterSeparator) {
  const std::vector<GuestMenuItem> items =
      BuildGuestMenuItems(/*is_editable=*/true, kAllEditFlags,
                          /*inspect_enabled=*/true);
  ASSERT_EQ(items.size(), 10u);
  EXPECT_EQ(items[8], (GuestMenuItem{Cmd::kSeparator, true}));
  EXPECT_EQ(items[9], (GuestMenuItem{Cmd::kInspect, true}));
}

}  // namespace
}  // namespace nexus
