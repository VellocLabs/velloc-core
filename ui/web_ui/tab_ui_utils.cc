#include "custom_browser/ui/web_ui/tab_ui_utils.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_style.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/tabs/tab.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_bubble_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_controller.h"
#include "chrome/browser/ui/views/tabs/tab_slot_controller.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/browser/ui/views/tabs/tab_strip_controller.h"
#include "ui/base/mojom/menu_source_type.mojom-shared.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace custom_browser {
namespace TabUIUtils {

namespace {

class ScopedHoverCardAnimationDisabler {
 public:
  ScopedHoverCardAnimationDisabler() {
    TabHoverCardController::set_disable_animations_for_testing(true);
  }
  ScopedHoverCardAnimationDisabler(const ScopedHoverCardAnimationDisabler&) =
      delete;
  ScopedHoverCardAnimationDisabler& operator=(
      const ScopedHoverCardAnimationDisabler&) = delete;
  ~ScopedHoverCardAnimationDisabler() {
    TabHoverCardController::set_disable_animations_for_testing(false);
  }
};

// Helper to attach a desired-bounds callback and snap once to avoid any
// initial flash.
void AttachFixedOriginDelegateAndSnap(views::Widget* widget,
                                      const gfx::Point& origin) {
  if (!widget || widget->IsClosed()) {
    return;
  }

  views::WidgetDelegate* delegate = widget->widget_delegate();
  if (!delegate) {
    return;
  }

  delegate->set_desired_bounds_delegate(base::BindRepeating(
      [](views::Widget* widget, gfx::Point target_origin) -> gfx::Rect {
        gfx::Size desired_size;
        if (views::View* contents = widget->GetContentsView()) {
          desired_size = contents->GetPreferredSize({});
        }
        if (desired_size.IsEmpty()) {
          desired_size = widget->GetWindowBoundsInScreen().size();
        }
        return gfx::Rect(target_origin, desired_size);
      },
      widget, origin));

  gfx::Rect bounds = widget->GetWindowBoundsInScreen();
  if (bounds.origin() != origin) {
    bounds.set_origin(origin);
    widget->SetBounds(bounds);
  }
}

bool TabIsValid(const TabContext& context) {
  if (context.lookup_result != TabLookupResult::kSuccess ||
      !context.tab_strip || !context.tab) {
    return false;
  }
  return true;
}

}  // namespace

TabContext PopulateTabContext(BrowserWindowInterface* expected_browser,
                              int32_t tab_id,
                              int32_t location_x,
                              int32_t location_y) {
  TabContext context;
  extensions::WindowController* window = nullptr;
  int tab_index = -1;
  if (!extensions::ExtensionTabUtil::GetTabById(
          tab_id, expected_browser->GetProfile(), /*include_incognito=*/true,
          &window, nullptr, &tab_index)) {
    return context;
  }

  if (!window) {
    return context;
  }

  BrowserWindowInterface* browser = window->GetBrowser();
  if (browser != expected_browser) {
    context.lookup_result = TabLookupResult::kDifferentBrowser;
    return context;
  }

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    return context;
  }


\
  context.tab_strip = browser_view->horizontal_tab_strip_for_testing();
  if (!context.tab_strip) {
    return context;
  }

  context.tab = context.tab_strip->tab_at(tab_index);
  if (!context.tab) {
    return context;
  }

  context.target_screen_point = gfx::Point(location_x, location_y);
  views::View::ConvertPointToScreen(browser_view, &context.target_screen_point);
  context.browser = browser;
  context.lookup_result = TabLookupResult::kSuccess;
  return context;
}

bool ShowTabContextMenuWithController(const TabContext& context) {
  if (!TabIsValid(context)) {
    return false;
  }

  auto* controller = context.tab_strip->controller();
  if (!controller) {
    return false;
  }

  controller->ShowContextMenuForTab(context.tab, context.target_screen_point,
                                    ui::mojom::MenuSourceType::kMouse);
  return true;
}

bool ShowTabFloatingPreview(const TabContext& context) {
  if (!TabIsValid(context)) {
    return false;
  }

  auto* hover_controller =
      context.tab_strip->hover_card_controller();
  if (!hover_controller) {
    return false;
  }
  context.tab->SetVisible(true);
  {
    ScopedHoverCardAnimationDisabler animation_disabler;
    TabHoverCardBubbleView* hover_card =
        hover_controller->hover_card_for_testing();
    if (hover_card) {
      hover_card->GetWidget()->Hide();
    }
    context.tab_strip->UpdateHoverCard(
        context.tab, TabSlotController::HoverCardUpdateType::kHover);
    hover_card = hover_controller->hover_card_for_testing();
    if (hover_card && hover_card->GetWidget() &&
        !hover_card->GetWidget()->IsClosed()) {
      views::Widget* hover_widget = hover_card->GetWidget();
      hover_widget->Show();
      // Install desired-bounds delegate and snap once to the target origin.
      AttachFixedOriginDelegateAndSnap(hover_widget,
                                       context.target_screen_point);
      return true;
    }
  }
  return false;
}

bool CloseTabFloatingPreview(const TabContext& context) {
  if (!TabIsValid(context)) {
    return false;
  }

  auto* hover_controller =
      context.tab_strip->hover_card_controller();
  if (!hover_controller) {
    return false;
  }

  // Best effort: clear any custom desired-bounds delegate before closing.
  if (TabHoverCardBubbleView* hover_card =
          hover_controller->hover_card_for_testing()) {
    if (views::Widget* w = hover_card->GetWidget(); w && !w->IsClosed()) {
      if (views::WidgetDelegate* delegate = w->widget_delegate()) {
        delegate->set_desired_bounds_delegate(
            base::RepeatingCallback<gfx::Rect()>());
      }
    }
  }

  if (!hover_controller->IsHoverCardVisible()) {
    return true;
  }

  hover_controller->UpdateHoverCard(
      nullptr, TabSlotController::HoverCardUpdateType::kEvent);
  hover_controller->PreventImmediateReshow();
  return true;
}

}  // namespace TabUIUtils
}  // namespace custom_browser
