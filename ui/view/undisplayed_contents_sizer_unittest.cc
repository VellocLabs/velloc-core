#include "custom_browser/ui/view/undisplayed_contents_sizer.h"

#include <optional>

#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/size.h"

namespace custom_browser {
namespace {

// The kNexus window's client area, and the guest's webview_area inside it.
constexpr gfx::Size kClientArea(1280, 1045);
constexpr gfx::Size kWebviewArea(1042, 1037);
// What a WebContents created while the contents container is hidden has.
constexpr gfx::Size kNeverSized(0, 0);

// The reported bug: an agent tab that no window shows (a scoped/remote-driven
// session, or a background session on the desktop) was left at 0x0. With no
// guest layout ever published, it must get the window's client area.
TEST(UndisplayedContentsSizerTest, NeverShownTabGetsClientArea) {
  UndisplayedContentsSizer sizer;
  EXPECT_EQ(std::optional<gfx::Size>(kClientArea),
            sizer.SizeFor(kNeverSized, kClientArea));
}

// Desktop background session: the user saw a tab at the webview_area size,
// then switched to a session without one. A new tab should open at the size
// it will have when the user switches back, not at the whole client area.
TEST(UndisplayedContentsSizerTest, PrefersTheLastShownSize) {
  UndisplayedContentsSizer sizer;
  sizer.OnContentsAreaLaidOut(kWebviewArea, /*visible=*/true);
  EXPECT_EQ(std::optional<gfx::Size>(kWebviewArea),
            sizer.SizeFor(kNeverSized, kClientArea));
}

// A hidden <main-contents-view> reports visible:false with a 0x0 rect. It
// must not wipe the remembered size.
TEST(UndisplayedContentsSizerTest, HiddenReportDoesNotForgetTheShownSize) {
  UndisplayedContentsSizer sizer;
  sizer.OnContentsAreaLaidOut(kWebviewArea, /*visible=*/true);
  sizer.OnContentsAreaLaidOut(gfx::Size(), /*visible=*/false);
  sizer.OnContentsAreaLaidOut(gfx::Size(500, 400), /*visible=*/false);
  EXPECT_EQ(kWebviewArea, sizer.last_shown_size());
  EXPECT_EQ(std::optional<gfx::Size>(kWebviewArea),
            sizer.SizeFor(kNeverSized, kClientArea));
}

// A visible but empty rect (the guest collapsed the splitter to nothing) is
// not a size a page can use either.
TEST(UndisplayedContentsSizerTest, VisibleEmptyRectIsNotRemembered) {
  UndisplayedContentsSizer sizer;
  sizer.OnContentsAreaLaidOut(gfx::Size(1042, 0), /*visible=*/true);
  EXPECT_TRUE(sizer.last_shown_size().IsEmpty());
  EXPECT_EQ(std::optional<gfx::Size>(kClientArea),
            sizer.SizeFor(kNeverSized, kClientArea));
}

// The latest shown size wins (window or splitter resized while shown).
TEST(UndisplayedContentsSizerTest, TracksTheLatestShownSize) {
  UndisplayedContentsSizer sizer;
  sizer.OnContentsAreaLaidOut(kWebviewArea, /*visible=*/true);
  sizer.OnContentsAreaLaidOut(gfx::Size(800, 600), /*visible=*/true);
  EXPECT_EQ(std::optional<gfx::Size>(gfx::Size(800, 600)),
            sizer.SizeFor(kNeverSized, kClientArea));
}

// A tab that already has a size is never resized: the one the user saw keeps
// its layout while hidden, and the NativeViewHost owns it once it is shown.
TEST(UndisplayedContentsSizerTest, SizedTabIsLeftAlone) {
  UndisplayedContentsSizer sizer;
  sizer.OnContentsAreaLaidOut(kWebviewArea, /*visible=*/true);
  EXPECT_EQ(std::nullopt, sizer.SizeFor(gfx::Size(640, 480), kClientArea));
  EXPECT_EQ(std::nullopt, sizer.SizeFor(kWebviewArea, kClientArea));
}

// Width or height 0 counts as unsized (gfx::Size::IsEmpty).
TEST(UndisplayedContentsSizerTest, HalfEmptyTabIsUnsized) {
  UndisplayedContentsSizer sizer;
  EXPECT_EQ(std::optional<gfx::Size>(kClientArea),
            sizer.SizeFor(gfx::Size(1042, 0), kClientArea));
  EXPECT_EQ(std::optional<gfx::Size>(kClientArea),
            sizer.SizeFor(gfx::Size(0, 1037), kClientArea));
}

// Before the window's first layout there is nothing to size to. Leave the tab
// alone; the next contents layout decides.
TEST(UndisplayedContentsSizerTest, NothingKnownYetDecidesNothing) {
  UndisplayedContentsSizer sizer;
  EXPECT_EQ(std::nullopt, sizer.SizeFor(kNeverSized, gfx::Size()));
}

}  // namespace
}  // namespace custom_browser
