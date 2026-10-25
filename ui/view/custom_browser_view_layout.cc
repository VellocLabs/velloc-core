#include "custom_browser/ui/view/custom_browser_view_layout.h"

#include <optional>

#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui_container_view.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/views/controls/separator.h"
namespace custom_browser {
CustomBrowserViewLayout::CustomBrowserViewLayout(
    std::unique_ptr<BrowserViewLayoutDelegate> delegate,
    BrowserWindowInterface* browser,
    BrowserViewLayoutViews views)
    : BrowserViewTabbedLayoutImpl(std::move(delegate), std::move(views)),
      browser_(browser) {}

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
    // A view with no parent is detached and owned by its creator (the
    // bookmark bar once hidden): adopting it would give it a second owner
    // and free it twice when the window is destroyed.
    if (!child || !child->parent() || IsParentedTo(child, new_parent)) {
      return;
    }
    new_parent->AddChildViewRaw(child);
    child->SetPaintToLayer();
  };

  fix_view_parent(views().bookmark_bar, custom_views().native_ui_container);
}

void CustomBrowserViewLayout::LayoutContentsContainerView(
    gfx::Rect& contents_container_bounds,
    bool visible) {
  FixViewsParentForCustom();

  BrowserLayoutParams params;
  params.visual_client_area = contents_container_bounds;
  const BrowserLayoutParams browser_params = params;

  // Lay out the bookmarks bar if one is present.
  const bool bookmarks_visible = visible && delegate().IsBookmarkBarVisible();
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

  // The multi-contents view draws the top separator unless an infobar sits
  // between it and the toolbar.
  views().multi_contents_view->SetShouldShowTopSeparator(
      !delegate().IsInfobarVisible());

  // Lay out infobar container.
  if (IsParentedTo(views().infobar_container, views().browser_view)) {
    gfx::Rect infobar_bounds;
    const bool infobar_visible = delegate().IsInfobarVisible();
    if (infobar_visible) {
      // Infobars slide down with top container reveal, but not when they're in
      // the toolbar-height side panel shadow box. This is because they only
      // slide down when they are visually adjacent to the toolbar/bookmarks
      // bar.
      const int additional_offset = delegate().GetExtraInfobarOffset();
      infobar_bounds =
          gfx::Rect(params.visual_client_area.x(),
                    params.visual_client_area.y() + additional_offset,
                    params.visual_client_area.width(),
                    // This returns zero for empty infobar.
                    views().infobar_container->GetPreferredSize().height());
      params.SetTop(infobar_bounds.bottom());
    }
    SetViewBoundAndVisibility(views().infobar_container, infobar_bounds,
                              visible && infobar_visible);
  }

  // Lay out contents-height side panel.
  bool contents_height_side_panel_leading = false;
  int min_contents_width = kContentsContainerMinimumWidth;

  // The contents-height side panel is adjusted for the presence of a top
  // container separator in the browser view.
  // TODO(max):views::Separator::kThickness
  const int contents_height_side_panel_top = params.visual_client_area.y();

  if (IsParentedTo(views().side_panel, views().browser_view)) {
    const SidePanel* const contents_height_side_panel =
        views().side_panel;
    int contents_height_side_panel_width = 0;
    int contents_height_side_panel_visible_width = 0;
    const bool is_right_aligned = contents_height_side_panel->IsRightAligned();
    contents_height_side_panel_leading =
        is_right_aligned == base::i18n::IsRTL();
    if (contents_height_side_panel->GetVisible()) {
      // Side panel implies a separator, which means we have to give a little
      // more room for the contents.
      min_contents_width += views::Separator::kThickness;

      // Maximum width is the lesser of preferred width and the largest width
      // that doesn't shrink the contents pane past its own minimum size.
      const int min_width =
          contents_height_side_panel->GetMinimumSize().width();
      const int preferred_width =
          contents_height_side_panel->GetPreferredSize().width();
      int max_width =
          std::min(preferred_width,
                   params.visual_client_area.width() - min_contents_width);

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
    const std::optional<bool> side_panel_visibility =
        visible ? std::nullopt : std::optional<bool>(false);
    SetViewBoundAndVisibility(views().side_panel,
                              contents_height_side_panel_bounds,
                              side_panel_visibility);
    params.InsetHorizontal(contents_height_side_panel_visible_width,
                           contents_height_side_panel_leading);
  }

  // Lay out contents container. The contents container contains the multi-
  // contents view when multi-contents are enabled. The checks here are to
  // force the logic to be updated when multi-contents is fully rolled-out.
  views::View* const contents_container =
      GetCustomBrowserView()->contents_container();
  CHECK(IsParentedTo(contents_container, views().browser_view));
  CHECK(contents_container->Contains(views().multi_contents_view));

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
  const gfx::Rect contents_container_rect(
      content_left, params.visual_client_area.y(), content_right - content_left,
      params.visual_client_area.height());
  SetViewBoundAndVisibility(contents_container, contents_container_rect,
                            visible);

  undisplayed_contents_sizer_.OnContentsAreaLaidOut(
      contents_container_rect.size(), visible);
  SizeUndisplayedTabs();
}

void CustomBrowserViewLayout::SizeUndisplayedTabs() {
  TabStripModel* const tab_strip_model =
      browser() ? browser()->GetTabStripModel() : nullptr;
  if (!tab_strip_model) {
    return;
  }
  // The native UI container spans the window's client area.
  const NativeUIContainerView* const native_ui_container =
      GetNativeUIContainerView();
  const gfx::Size client_area =
      native_ui_container ? native_ui_container->size() : gfx::Size();
  for (int i = 0; i < tab_strip_model->count(); ++i) {
    content::WebContents* const contents =
        tab_strip_model->GetWebContentsAt(i);
    if (!contents || contents->IsBeingDestroyed()) {
      continue;
    }
    const std::optional<gfx::Size> size =
        undisplayed_contents_sizer_.SizeFor(contents->GetSize(), client_area);
    if (size) {
      contents->Resize(gfx::Rect(*size));
    }
  }
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


  // Upstream decorations only the Chromium layout places; once shown they would
  // stay shown, stale, in every custom mode. Our layout draws its own, so keep
  // them hidden.
  if (!is_chrome_layout) {
    if (views().main_shadow_overlay) {
      views().main_shadow_overlay->SetVisible(false);
    }
    if (views().main_background_region) {
      views().main_background_region->SetVisible(false);
    }
  }

  if (views().horizontal_tab_strip_region_view) {
    views().horizontal_tab_strip_region_view->SetVisible(show_top_container);
  }
  if (views().vertical_tab_strip_region_view) {
    views().vertical_tab_strip_region_view->SetVisible(show_top_container);
  }
  if (views().top_container) {
    views().top_container->SetVisible(show_top_container);
  }
  return layout;
}

}  // namespace custom_browser
