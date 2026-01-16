#include "custom_browser/ui/view/mouse_event_handler.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/events/types/event_type.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/wm/core/coordinate_conversion.h"

MouseEventHandler::RectMouseWatcherHost::RectMouseWatcherHost(
    const views::View* view,
    const gfx::Rect& view_bounds)
    : view_(view), view_bounds_(view_bounds) {
  DCHECK(view_);
}

MouseEventHandler::RectMouseWatcherHost::~RectMouseWatcherHost() = default;

bool MouseEventHandler::RectMouseWatcherHost::Contains(
    const gfx::Point& screen_point,
    EventType type) {
  gfx::Point origin_in_screen = view_bounds_.origin();
  views::View::ConvertPointToScreen(view_, &origin_in_screen);
  gfx::Rect screen_bounds(origin_in_screen, view_bounds_.size());

  const bool in_rect = screen_bounds.Contains(screen_point);
  if (!in_rect || (type == EventType::kExit && !IsMouseOverWindow())) {
    return false;
  }
  return true;
}

bool MouseEventHandler::RectMouseWatcherHost::IsMouseOverWindow() const {
  const views::Widget* widget = view_->GetWidget();
  if (!widget) {
    return false;
  }
  display::Screen* screen = display::Screen::Get();
  return screen && screen->IsWindowUnderCursor(widget->GetNativeWindow());
}

MouseEventHandler::RequestListener::RequestListener(MouseEventHandler* owner)
    : owner_(owner) {}

MouseEventHandler::RequestListener::~RequestListener() = default;

void MouseEventHandler::RequestListener::MouseMovedOutOfHost() {
  if (owner_) {
    owner_->OnWatcherMovedOut();
  }
}

MouseEventHandler::MouseEventHandler(CustomBrowserView* custom_browser_view,
                                     aura::Window* root_window)
    : custom_browser_view_(custom_browser_view), root_window_(root_window) {
  DCHECK(custom_browser_view_);
  DCHECK(root_window_);
  root_window_observation_.Observe(root_window_);
  root_window_->AddPreTargetHandler(this);
}

MouseEventHandler::~MouseEventHandler() {
  StopCapture(/*success=*/false);
  StopObserving();
}

void MouseEventHandler::OnWindowDestroying(aura::Window* window) {
  DCHECK_EQ(window, root_window_);
  StopObserving();
}

void MouseEventHandler::StopObserving() {
  if (!root_window_observation_.IsObserving()) {
    return;
  }
  root_window_observation_.GetSource()->RemovePreTargetHandler(this);
  root_window_observation_.Reset();
  root_window_ = nullptr;
}

void MouseEventHandler::OnMouseEvent(ui::MouseEvent* event) {
  DCHECK(event);

  const ui::LocatedEvent* located = event->AsLocatedEvent();
  if (!located) {
    return;
  }

  gfx::Point location_in_view = located->root_location();
  wm::ConvertPointToScreen(root_window_, &location_in_view);
  views::View::ConvertPointFromScreen(custom_browser_view_, &location_in_view);

  DVLOG(1) << "Mouse event type=" << static_cast<int>(event->type()) << " at "
           << location_in_view.ToString();

  if (capture_active_) {
    const bool inside = capture_bounds_.Contains(location_in_view);
    HandlePointerInside(inside);
  }

  custom_browser_view_->OnPreHandleRootViewMouseEvent(location_in_view, event);
}

bool MouseEventHandler::StartCapture(
    const gfx::Rect& bounds,
    base::TimeDelta delay,
    base::OnceCallback<void(bool)> completion_callback) {
  if (completion_callback.is_null()) {
    return false;
  }

  // Cancel any in-flight capture and report it as aborted.
  StopCapture(/*success=*/false);

  completion_ = std::move(completion_callback);
  max_outside_delay_ = delay;
  pointer_inside_ = true;

  DCHECK(custom_browser_view_);
  views::Widget* widget = custom_browser_view_->GetWidget();
  if (!widget) {
    CompleteCapture(/*success=*/false);
    return false;
  }

  gfx::NativeWindow native = widget->GetNativeWindow();
  if (!native) {
    CompleteCapture(/*success=*/false);
    return false;
  }

  aura::Window* root_window = native->GetRootWindow();
  if (!root_window) {
    CompleteCapture(/*success=*/false);
    return false;
  }

  DCHECK_EQ(root_window_, root_window);

  gfx::Rect capture_bounds = bounds;
  if (custom_browser_view_->custom_browser_webui_view_) {
    capture_bounds = views::View::ConvertRectToTarget(
        custom_browser_view_->custom_browser_webui_view_.get(),
        custom_browser_view_, capture_bounds);
  }

  const gfx::Rect local_bounds = custom_browser_view_->GetLocalBounds();
  if (capture_bounds.IsEmpty()) {
    capture_bounds = local_bounds;
  } else {
    gfx::Rect clipped_bounds = capture_bounds;
    clipped_bounds.Intersect(local_bounds);
    if (clipped_bounds.IsEmpty()) {
      CompleteCapture(/*success=*/false);
      return false;
    }
    capture_bounds = clipped_bounds;
  }

  capture_bounds_ = capture_bounds;
  BeginCapture(native);

  return true;
}

void MouseEventHandler::StopCapture(bool success) {
  outside_timer_.Stop();
  if (!completion_ && !capture_active_) {
    return;
  }

  CompleteCapture(success);
}

void MouseEventHandler::BeginCapture(gfx::NativeWindow native_window) {
  if (!completion_) {
    return;
  }

  listener_ = std::make_unique<RequestListener>(this);

  auto host = std::make_unique<RectMouseWatcherHost>(custom_browser_view_,
                                                     capture_bounds_);

  watcher_ =
      std::make_unique<views::MouseWatcher>(std::move(host), listener_.get());
  watcher_->Start(native_window);
  capture_active_ = true;
}

void MouseEventHandler::OnWatcherMovedOut() {
  HandlePointerInside(/*inside=*/false);
}

void MouseEventHandler::HandlePointerInside(bool inside) {
  if (!capture_active_) {
    return;
  }

  if (inside) {
    pointer_inside_ = true;
    outside_timer_.Stop();
    return;
  }

  if (!pointer_inside_ && outside_timer_.IsRunning()) {
    return;
  }

  pointer_inside_ = false;
  if (max_outside_delay_.is_zero()) {
    CompleteCapture(/*success=*/true);
    return;
  }

  outside_timer_.Start(
      FROM_HERE, max_outside_delay_,
      base::BindOnce(&MouseEventHandler::CompleteCapture,
                     weak_ptr_factory_.GetWeakPtr(), /*success=*/true));
}

void MouseEventHandler::CompleteCapture(bool success) {
  outside_timer_.Stop();
  watcher_.reset();
  listener_.reset();
  capture_active_ = false;

  if (completion_) {
    std::move(completion_).Run(success);
  }
}
