#include "custom_browser/ui/view/view_visible_bounds.h"

#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/test/views/chrome_views_test_base.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/test/views_test_utils.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace custom_browser {
namespace {

// Builds widget > root > container > anchor, mirroring the kNexus window:
// root stands in for the BrowserView, container for the contents container
// that CustomBrowserViewLayout::LayoutContentsContainerView positions and
// hides, anchor for the contents WebView the handoff button is anchored to.
// container and anchor start the way Chromium creates them: visible, 0x0.
class ViewVisibleBoundsTest : public ChromeViewsTestBase {
 protected:
  void SetUp() override {
    ChromeViewsTestBase::SetUp();
    widget_ = CreateTestWidget(views::Widget::InitParams::CLIENT_OWNS_WIDGET);
    widget_->SetBounds(gfx::Rect(0, 0, 800, 600));
    root_ = widget_->SetContentsView(std::make_unique<views::View>());
    container_ = root_->AddChildView(std::make_unique<views::View>());
    anchor_ = container_->AddChildView(std::make_unique<views::View>());
    widget_->Show();
    views::test::RunScheduledLayout(widget_.get());
    ASSERT_FALSE(root_->bounds().IsEmpty());
  }

  void TearDown() override {
    anchor_ = nullptr;
    container_ = nullptr;
    root_ = nullptr;
    widget_.reset();
    ChromeViewsTestBase::TearDown();
  }

  // What LayoutContentsContainerView does with a <main-contents-view> report:
  // position the container, show or hide it. The anchor fills the container,
  // as the contents WebView does.
  void LayOutContainer(const gfx::Rect& bounds, bool visible) {
    container_->SetBoundsRect(bounds);
    container_->SetVisible(visible);
    anchor_->SetBoundsRect(gfx::Rect(bounds.size()));
  }

  std::unique_ptr<views::Widget> widget_;
  raw_ptr<views::View> root_ = nullptr;
  raw_ptr<views::View> container_ = nullptr;
  raw_ptr<views::View> anchor_ = nullptr;
};

TEST_F(ViewVisibleBoundsTest, NullHasNoVisibleBounds) {
  EXPECT_FALSE(HasVisibleBounds(nullptr));
}

TEST_F(ViewVisibleBoundsTest, ShownContainerIsOnScreen) {
  LayOutContainer(gfx::Rect(400, 0, 400, 600), /*visible=*/true);
  EXPECT_TRUE(HasVisibleBounds(anchor_));
  EXPECT_TRUE(HasVisibleBounds(container_));
}

// The reported bug: no <main-contents-view> layout has reached the container
// yet, so it is still visible at 0x0. Every view in the chain is visible, so a
// view-chain visibility check said "on screen" and the handoff button floated
// over the chat for a tab nobody sees.
TEST_F(ViewVisibleBoundsTest, NeverLaidOutContainerIsNotOnScreen) {
  ASSERT_TRUE(container_->GetVisible());
  ASSERT_TRUE(anchor_->GetVisible());
  ASSERT_TRUE(container_->bounds().IsEmpty());
  EXPECT_FALSE(HasVisibleBounds(anchor_));
}

// A 0x0 container clips a child that still has a size of its own.
TEST_F(ViewVisibleBoundsTest, ZeroSizedContainerClipsASizedAnchor) {
  anchor_->SetBoundsRect(gfx::Rect(0, 0, 400, 600));
  ASSERT_TRUE(container_->bounds().IsEmpty());
  EXPECT_FALSE(HasVisibleBounds(anchor_));
}

// A background session: the layout hid the container for a session that owns
// no tab here. The anchor keeps its own visible flag and its last size.
TEST_F(ViewVisibleBoundsTest, HiddenContainerIsNotOnScreen) {
  LayOutContainer(gfx::Rect(400, 0, 400, 600), /*visible=*/true);
  ASSERT_TRUE(HasVisibleBounds(anchor_));
  container_->SetVisible(false);
  EXPECT_TRUE(anchor_->GetVisible());
  EXPECT_FALSE(anchor_->bounds().IsEmpty());
  EXPECT_FALSE(HasVisibleBounds(anchor_));
}

// The hidden <main-contents-view> report: visible:false with a 0x0 rect.
TEST_F(ViewVisibleBoundsTest, HiddenReportIsNotOnScreen) {
  LayOutContainer(gfx::Rect(), /*visible=*/false);
  EXPECT_FALSE(HasVisibleBounds(anchor_));
}

// Switching back to the session that owns the tab shows it again.
TEST_F(ViewVisibleBoundsTest, ReshowingContainerIsOnScreenAgain) {
  LayOutContainer(gfx::Rect(), /*visible=*/false);
  ASSERT_FALSE(HasVisibleBounds(anchor_));
  LayOutContainer(gfx::Rect(400, 0, 400, 600), /*visible=*/true);
  EXPECT_TRUE(HasVisibleBounds(anchor_));
}

// A container positioned entirely outside the window area shows nothing.
TEST_F(ViewVisibleBoundsTest, ContainerClippedOutOfTheWindowIsNotOnScreen) {
  LayOutContainer(gfx::Rect(900, 0, 400, 600), /*visible=*/true);
  EXPECT_FALSE(HasVisibleBounds(anchor_));
}

TEST_F(ViewVisibleBoundsTest, ViewOutsideAWidgetIsNotOnScreen) {
  views::View orphan;
  orphan.SetBoundsRect(gfx::Rect(0, 0, 100, 100));
  EXPECT_FALSE(HasVisibleBounds(&orphan));
}

}  // namespace
}  // namespace custom_browser
