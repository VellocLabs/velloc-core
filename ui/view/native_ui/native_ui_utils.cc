#include "custom_browser/ui/view/native_ui/native_ui_utils.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/tabs/tab_search_container.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/create_toolbar_button_native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/pinned_toolbar_actions_container_native_ui.h"
#include "ui/actions/action_id.h"
#include "ui/base/class_property.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/background.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_tracker.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace {
class NativeUIViewLayoutObserver;
struct SavedViewHierarchyNode {
  SavedViewHierarchyNode() = default;
  ~SavedViewHierarchyNode() = default;
  views::ViewTracker tracker;
  bool was_visible = false;
  std::vector<std::unique_ptr<SavedViewHierarchyNode>> children;
};

enum class NativeUIViewPoolType {
  kNone = 0,
  kToolbarDivider,
  kPinnedToolbarButton,
};

struct NativeUIViewPoolState {
  std::string class_name;
  bool used = false;
};

}  // namespace

DEFINE_UI_CLASS_PROPERTY_TYPE(NativeUIViewPoolState*)
DEFINE_UI_CLASS_PROPERTY_TYPE(NativeUIViewLayoutObserver*)
DEFINE_UI_CLASS_PROPERTY_TYPE(SavedViewHierarchyNode*)
DEFINE_UI_CLASS_PROPERTY_TYPE(ToolbarButton*)

namespace {
constexpr int kMaxSavedViewHierarchyDepth = 8;

bool ShouldStopSearchAtView(const views::View* view) {
  const auto class_name = view->GetClassName();
  if (class_name == LocationBarView::kViewClassName) {
    return true;
  }
  if (class_name.ends_with("Button")) {
    return true;
  }
  return false;
}

std::unique_ptr<SavedViewHierarchyNode> BuildSavedViewHierarchy(
    views::View* root,
    int depth,
    CustomBrowserView* browser_view) {
  auto node = std::make_unique<SavedViewHierarchyNode>();
  node->tracker.SetView(root);
  node->was_visible = root->GetVisible();
  if (depth + 1 >= kMaxSavedViewHierarchyDepth) {
    return node;
  }
  if (ShouldStopSearchAtView(root)) {
    return node;
  }
  ++depth;
  if (root == browser_view) {
    auto top_node = BuildSavedViewHierarchy(browser_view->top_container(),
                                            depth, browser_view);
    node->children.push_back(std::move(top_node));

    auto tab_strip_node = BuildSavedViewHierarchy(
        browser_view->tab_strip_view(), depth, browser_view);
    node->children.push_back(std::move(tab_strip_node));
    return node;
  }

  node->children.reserve(root->children().size());
  for (views::View* child : root->children()) {
    auto child_node = BuildSavedViewHierarchy(child, depth, browser_view);
    node->children.push_back(std::move(child_node));
  }
  return node;
}

void RestoreViewHierarchy(CustomBrowserView* browser_view,
                          SavedViewHierarchyNode* node) {
  views::View* parent_view = node->tracker.view();
  size_t insert_index = 0;
  for (const auto& child_node : node->children) {
    views::View* child_view = child_node->tracker.view();
    if (!child_view) {
      continue;
    }
    if (parent_view) {
      if (child_view->parent() != parent_view) {
        DVLOG(1) << __FUNCTION__ << " restore:" << child_view->GetClassName()
                 << " parent: " << parent_view->GetClassName()
                 << " index:" << insert_index;
        parent_view->AddChildViewAt(child_view, insert_index);
        child_view->SetVisible(true);
      } else if (parent_view != browser_view) {
        parent_view->ReorderChildView(child_view, insert_index);
      }
    }
    RestoreViewHierarchy(browser_view, child_node.get());
    ++insert_index;
  }
}

class NativeUIViewLayoutObserver : public views::ViewObserver {
 public:
  NativeUIViewLayoutObserver(custom_browser::native_ui::NativeUI* native_ui,
                             views::View* view)
      : native_ui_(native_ui), view_(view) {
    DCHECK(native_ui_);
    DCHECK(view_);
    view_->AddObserver(this);
  }
  NativeUIViewLayoutObserver(const NativeUIViewLayoutObserver&) = delete;
  NativeUIViewLayoutObserver& operator=(const NativeUIViewLayoutObserver&) =
      delete;
  ~NativeUIViewLayoutObserver() override {
    if (view_) {
      OnViewIsDeleting(view_);
    }
  }

  void OnViewLayoutInvalidated(views::View*) override { NotifyLayout(); }

  void OnViewIsDeleting(views::View*) override {
    view_->RemoveObserver(this);
    view_ = nullptr;
    native_ui_ = nullptr;
  }

 private:
  void NotifyLayout() {
    if (in_layout_) {
      return;
    }
    base::AutoReset<bool> auto_reset(&in_layout_, true);
    if (native_ui_) {
      native_ui_->CalculateBounds();
      native_ui_->Layout();
    }
  }

  bool in_layout_ = false;
  raw_ptr<custom_browser::native_ui::NativeUI> native_ui_;
  raw_ptr<views::View> view_;
};
DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(NativeUIViewLayoutObserver,
                                   kNativeUIViewBoundsObserverKey)
DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(SavedViewHierarchyNode,
                                   kSavedViewHierarchyNodeKey)
DEFINE_UI_CLASS_PROPERTY_KEY(ToolbarButton*, kSavedDownloadButtonKey, nullptr)
DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(NativeUIViewPoolState,
                                   kNativeUIViewPoolStateKey)
}  // namespace

namespace custom_browser::native_ui::utils {

NativeUIViewPoolState* GetOrCreatePoolState(views::View* view) {
  DCHECK(view);
  auto* state = view->GetProperty(kNativeUIViewPoolStateKey);
  if (!state) {
    state =
        view->SetProperty(kNativeUIViewPoolStateKey, NativeUIViewPoolState());
  }
  return state;
}

void PrintViewHierarchyLog(CustomBrowserView* browser_view) {
#if DCHECK_IS_ON()
  if (auto* widget = browser_view->GetWidget()) {
    DVLOG(2) << __FUNCTION__
             << "ViewHierarchy: " << PrintViewHierarchy(widget->GetRootView());
  }
#endif
}

bool SaveChromiumHierarchy(CustomBrowserView* browser_view) {
  DCHECK(browser_view);
  views::View* root = browser_view->parent();
  if (!root) {
    browser_view->ClearProperty(kSavedViewHierarchyNodeKey);
    return false;
  }

  auto hierarchy = BuildSavedViewHierarchy(root, 0, browser_view);
  if (!hierarchy) {
    browser_view->ClearProperty(kSavedViewHierarchyNodeKey);
    return false;
  }

  browser_view->SetProperty(kSavedViewHierarchyNodeKey, std::move(hierarchy));
  PrintViewHierarchyLog(browser_view);
  return true;
}

bool RestoreChromiumHierarchy(CustomBrowserView* browser_view) {
  DCHECK(browser_view);
  auto* hierarchy = browser_view->GetProperty(kSavedViewHierarchyNodeKey);
  if (!hierarchy) {
    return false;
  }
  RestoreViewHierarchy(browser_view, hierarchy);
  return true;
}

void StartObservingViewLayout(NativeUI* native_ui, views::View* view) {
  view->SetProperty(
      kNativeUIViewBoundsObserverKey,
      std::make_unique<NativeUIViewLayoutObserver>(native_ui, view));
}

void StopObservingViewLayout(views::View* view) {
  view->ClearProperty(kNativeUIViewBoundsObserverKey);
}

void SaveDownloadButton(views::View* browser_view,
                        ToolbarButton* download_button) {
  DCHECK(browser_view);
  browser_view->SetProperty(kSavedDownloadButtonKey, download_button);
}

ToolbarButton* GetDownloadsButton(BrowserView* browser_view) {
  DCHECK(browser_view);
  return browser_view->GetProperty(kSavedDownloadButtonKey);
}

bool IsChromiumUI(BrowserView* browser_view) {
  DCHECK(browser_view);
  auto* custom_browser_view = static_cast<CustomBrowserView*>(browser_view);
  return custom_browser_view->IsChromiumUI();
}

TabStripRegionView* FindTabStripRegionView(BrowserView* browser_view) {
  // In custom mode TabStripRegionView may be detached from root hierarchy.
  // Search from tab strip pointer first.
  if (auto* tab_strip_view = browser_view->tab_strip_view()) {
    return static_cast<TabStripRegionView*>(tab_strip_view);
  }
  return nullptr;
}

std::unique_ptr<views::View> CreateDividerView(bool use_dark_color) {
  auto divider = std::make_unique<views::View>();
  divider->SetPreferredSize(
      gfx::Size(GetLayoutConstant(TOOLBAR_DIVIDER_WIDTH),
                GetLayoutConstant(TOOLBAR_DIVIDER_HEIGHT)));
  divider->SetBackground(views::CreateRoundedRectBackground(
      use_dark_color ? kColorToolbarButtonIconDisabled
                     : kColorToolbarExtensionSeparatorEnabled,
      GetLayoutConstant(TOOLBAR_DIVIDER_CORNER_RADIUS)));
  divider->SetPaintToLayer();
  return divider;
}

views::View* FindUnusedView(views::View* parent_view,
                            const std::string& view_class_name) {
  for (views::View* child : parent_view->children()) {
    const auto* state = child->GetProperty(kNativeUIViewPoolStateKey);
    if (state && !state->used && state->class_name == view_class_name) {
      return child;
    }
  }
  return nullptr;
}

void MarkViewUsed(views::View* view, const std::string& view_class_name) {
  if (!view) {
    return;
  }
  auto* state = GetOrCreatePoolState(view);
  state->class_name = view_class_name;
  state->used = true;
}

void ReleaseView(views::View* view) {
  if (!view) {
    return;
  }
  auto* state = view->GetProperty(kNativeUIViewPoolStateKey);
  if (state) {
    state->used = false;
  }
  view->SetVisible(false);
}

views::View* FindViewByClassInHierarchy(views::View* root,
                                        const std::string& class_name) {
  if (root->GetClassName() == class_name) {
    return root;
  }
  for (const auto& child : root->children()) {
    auto* result = FindViewByClassInHierarchy(child, class_name);
    if (result) {
      return result;
    }
  }
  return nullptr;
}

}  // namespace custom_browser::native_ui::utils
