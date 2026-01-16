#include "custom_browser/ui/view/custom_browser_view_layout.h"

#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui_container_view.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"
namespace {

// Loading bar is thicker than a separator, but instead of moving the bottom
// of the top container down, it starts above where the separator would go.
static constexpr int kLoadingBarHeight = 3;
static constexpr int kLoadingBarOffset =
    kLoadingBarHeight - views::Separator::kThickness;

}  // namespace

namespace custom_browser {
CustomBrowserViewLayout::CustomBrowserViewLayout(
    std::unique_ptr<BrowserViewLayoutDelegate> delegate,
    Browser* browser,
    BrowserViewLayoutViews views)
    : BrowserViewTabbedLayoutImpl(std::move(delegate),
                                  browser,
                                  std::move(views)) {}

CustomBrowserViewLayout::~CustomBrowserViewLayout() {}

CustomBrowserView* CustomBrowserViewLayout::GetCustomBrowserView() const {
  if (!views().browser_view) {
    return nullptr;
  }
  return static_cast<CustomBrowserView*>(views().browser_view);
}

CustomBrowserViews& CustomBrowserViewLayout::custom_views() const {
  DCHECK(views().browser_view);
  return CustomBrowserView::FromBrowserView(views().browser_view);
}

NativeUIContainerView* CustomBrowserViewLayout::GetNativeUIContainerView()
    const {
  auto* custom_browser_view = GetCustomBrowserView();
  return custom_browser_view ? custom_browser_view->native_ui_container()
                             : nullptr;
}

void CustomBrowserViewLayout::SetViewBoundAndVisibility(
    views::View* child,
    const gfx::Rect& bounds_,
    std::optional<bool> visibility_) const {
  DCHECK(child);
  child->SetBoundsRect(bounds_);
  if (visibility_) {
    child->SetVisible(*visibility_);
  }
}

void CustomBrowserViewLayout::FixViewsParentForCustom() {
  auto fix_view_parent = [](views::View* child, views::View* new_parent) {
    if (!child || IsParentedTo(child, new_parent)) {
      return;
    }
    new_parent->AddChildViewRaw(child);
    child->SetPaintToLayer();
  };

  fix_view_parent(views().bookmark_bar, custom_views().native_ui_container);
  fix_view_parent(views().loading_bar, custom_views().native_ui_container);
}

void CustomBrowserViewLayout::LayoutContentsContainerView(
    gfx::Rect& contents_container_bounds) {
  FixViewsParentForCustom();

  BrowserLayoutParams params;
  params.visual_client_area = contents_container_bounds;
  const BrowserLayoutParams browser_params = params;
  const bool show_shadow_overlay = ShadowOverlayVisible();

  // Lay out the bookmarks bar if one is present.
  const bool bookmarks_visible = delegate().IsBookmarkBarVisible();
  if (IsParentedTo(views().bookmark_bar, custom_views().native_ui_container)) {
    const gfx::Rect bookmarks_bounds(
        params.visual_client_area.x(), params.visual_client_area.y(),
        params.visual_client_area.width(),
        bookmarks_visible ? views().bookmark_bar->GetPreferredSize().height()
                          : 0);
    auto* bookmark_bar_view =
        static_cast<BookmarkBarView*>(views().bookmark_bar);
    // Remove bookmark_bart temporarily to calculate correct bounds.
    base::AutoReset reset(&bookmark_bar_view->browser_view_, nullptr);
    SetViewBoundAndVisibility(views().bookmark_bar, bookmarks_bounds,
                              bookmarks_visible);
    params.SetTop(bookmarks_bounds.bottom());
  }

  // There are multiple different ways the top separator can render.
  const TopSeparatorType top_separator_type = GetTopSeparatorType();

  // Lay out the loading bar when present.
  if (IsParentedTo(views().loading_bar, custom_views().native_ui_container)) {
    gfx::Rect loading_bar_bounds;
    if (top_separator_type == TopSeparatorType::kLoadingBar) {
      loading_bar_bounds =
          gfx::Rect(params.visual_client_area.x(),
                    params.visual_client_area.y() - kLoadingBarOffset,
                    params.visual_client_area.width(), kLoadingBarHeight);
      params.SetTop(loading_bar_bounds.bottom());
    }
    SetViewBoundAndVisibility(
        views().loading_bar, loading_bar_bounds,
        top_separator_type == TopSeparatorType::kLoadingBar);
  }

  // Maybe show the separator in the multi-contents view.
  views().multi_contents_view->SetShouldShowTopSeparator(
      top_separator_type == TopSeparatorType::kMultiContents);

  // Lay out infobar container.
  if (IsParentedTo(views().infobar_container, views().browser_view)) {
    gfx::Rect infobar_bounds;
    const bool infobar_visible = delegate().IsInfobarVisible();
    if (infobar_visible) {
      // Infobars slide down with top container reveal, but not when they're in
      // the toolbar-height side panel shadow box. This is because they only
      // slide down when they are visually adjacent to the toolbar/bookmarks
      // bar.
      const int additional_offset =
          show_shadow_overlay ? 0 : delegate().GetExtraInfobarOffset();
      infobar_bounds =
          gfx::Rect(params.visual_client_area.x(),
                    params.visual_client_area.y() + additional_offset,
                    params.visual_client_area.width(),
                    // This returns zero for empty infobar.
                    views().infobar_container->GetPreferredSize().height());
      params.SetTop(infobar_bounds.bottom());
    }
    SetViewBoundAndVisibility(views().infobar_container, infobar_bounds,
                              infobar_visible);
  }

  // Lay out contents-height side panel.
  bool show_leading_separator = false;
  bool show_trailing_separator = false;
  bool contents_height_side_panel_leading = false;
  int min_contents_width = kContentsContainerMinimumWidth;

  // The contents-height side panel is adjusted for the presence of a top
  // container separator in the browser view.
  // TODO(max):views::Separator::kThickness
  const int contents_height_side_panel_top = params.visual_client_area.y();

  if (IsParentedTo(views().contents_height_side_panel, views().browser_view)) {
    const SidePanel* const contents_height_side_panel =
        views().contents_height_side_panel;
    int contents_height_side_panel_width = 0;
    int contents_height_side_panel_visible_width = 0;
    const bool is_right_aligned = contents_height_side_panel->IsRightAligned();
    contents_height_side_panel_leading =
        is_right_aligned == base::i18n::IsRTL();
    if (contents_height_side_panel->GetVisible()) {
      // Side panel implies a separator, which means we have to give a little
      // more room for the contents.
      min_contents_width += views::Separator::kThickness;
      show_leading_separator = contents_height_side_panel_leading;
      show_trailing_separator = !contents_height_side_panel_leading;

      // Maximum width is the lesser of preferred width and the largest width
      // that doesn't shrink the contents pane past its own minimum size.
      const int min_width =
          contents_height_side_panel->GetMinimumSize().width();
      const int preferred_width =
          contents_height_side_panel->GetPreferredSize().width();
      int max_width =
          std::min(preferred_width,
                   params.visual_client_area.width() - min_contents_width);
      if (contents_height_side_panel->ShouldRestrictMaxWidth()) {
        max_width =
            std::min(max_width, params.visual_client_area.width() * 2 / 3);
      }

      // Side panel always gets at least its minimum width.
      contents_height_side_panel_width = std::max(min_width, max_width);
      contents_height_side_panel_visible_width =
          base::ClampFloor(contents_height_side_panel_width *
                           contents_height_side_panel->GetAnimationValue());
    }

    // Side panel slides in from the edge of the main container.
    const gfx::Rect contents_height_side_panel_bounds(
        contents_height_side_panel_leading
            ? params.visual_client_area.x() -
                  (contents_height_side_panel_width -
                   contents_height_side_panel_visible_width)
            : params.visual_client_area.right() -
                  contents_height_side_panel_visible_width,
        contents_height_side_panel_top, contents_height_side_panel_width,
        params.visual_client_area.bottom() - contents_height_side_panel_top);
    SetViewBoundAndVisibility(views().contents_height_side_panel,
                              contents_height_side_panel_bounds);
    params.InsetHorizontal(contents_height_side_panel_visible_width,
                           contents_height_side_panel_leading);
  }

  // Show separators in multi-contents view. Note that the multi-contents
  // view is inside the main container so doesn't need to be laid out.
  views().multi_contents_view->SetShouldShowLeadingSeparator(
      show_leading_separator);
  views().multi_contents_view->SetShouldShowTrailingSeparator(
      show_trailing_separator);

  // Lay out contents container. The contents container contains the multi-
  // contents view when multi-contents are enabled. The checks here are to
  // force the logic to be updated when multi-contents is fully rolled-out.
  CHECK(
      IsParentedToAndVisible(views().contents_container, views().browser_view));
  CHECK(views().contents_container->Contains(views().multi_contents_view));

  // Because side panels have minimum width, in a small browser, it is possible
  // for the combination of minimum-sized contents pane and minimum-sized side
  // panel may exceed the width of the window. In this case, the contents pane
  // slides under the side panel.
  int content_left = params.visual_client_area.x();
  int content_right = params.visual_client_area.right();
  if (const int deficit =
          min_contents_width - params.visual_client_area.width();
      deficit > 0) {
    // Expand the contents by the deficit on the side with the side panel.
    params.InsetHorizontal(-deficit, contents_height_side_panel_leading);
    // However, do not let this go past the edge of the allowed area.
    content_left =
        std::max(content_left, browser_params.visual_client_area.x());
    content_right =
        std::min(content_right, browser_params.visual_client_area.right());
  }
  SetViewBoundAndVisibility(
      views().contents_container,
      gfx::Rect(content_left, params.visual_client_area.y(),
                content_right - content_left,
                params.visual_client_area.height()));
}

CustomBrowserViewLayout::ProposedLayout
CustomBrowserViewLayout::CalculateProposedLayout(
    const BrowserLayoutParams& params) const {
  ProposedLayout layout;
  const bool is_full_screen =
      GetCustomBrowserView()->IsFullscreen();
  const bool is_chrome_layout =
      GetCustomBrowserView()->IsChromiumLayout();
  const bool show_top_container = is_chrome_layout &&
                                  !is_full_screen;
  if (is_chrome_layout) {
    layout = BrowserViewTabbedLayoutImpl::CalculateProposedLayout(params);
  } else {
    // Force layout update of native UI container, even when the rects are the
    // same. When infobar show/hide, the native UI container needs to relayout,
    // but the rects of native UI container may not change.
    custom_views().native_ui_container->InvalidateLayout(true);
  }

  layout.AddChild(custom_views().native_ui_container, params.visual_client_area,
                  !is_chrome_layout);
  layout.AddChild(custom_views().custom_browser_webui_view,
                  params.visual_client_area, !is_chrome_layout);


  if (views().tab_strip_region_view) {
    views().tab_strip_region_view->SetVisible(show_top_container);
  }
  if (views().vertical_tab_strip_container) {
    views().vertical_tab_strip_container->SetVisible(show_top_container);
  }
  if (views().top_container) {
    views().top_container->SetVisible(show_top_container);
  }
  return layout;
}

}  // namespace custom_browser
