#ifndef CUSTOM_BROWSER_UI_VIEW_MOUSE_EVENT_HANDLER_H_
#define CUSTOM_BROWSER_UI_VIEW_MOUSE_EVENT_HANDLER_H_

#include <memory>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "ui/aura/window.h"
#include "ui/aura/window_observer.h"
#include "ui/events/event_handler.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/mouse_watcher.h"
#include "ui/views/view.h"

class CustomBrowserView;

// Watches the mouse using views::MouseWatcher (timer + global cursor) so that
// you still get a completion callback even after the mouse leaves the window.
class MouseEventHandler : public ui::EventHandler, public aura::WindowObserver {
 public:
  MouseEventHandler(CustomBrowserView* custom_browser_view,
                    aura::Window* root_window);
  MouseEventHandler(const MouseEventHandler&) = delete;
  MouseEventHandler& operator=(const MouseEventHandler&) = delete;
  ~MouseEventHandler() override;

  // ui::EventHandler:
  void OnMouseEvent(ui::MouseEvent* event) override;

  // Begin watching `bounds` (in `custom_browser_view_`'s coordinate space).
  // The callback runs with `true` when the pointer leaves `bounds`;
  // `StopCapture(false)` reports an early cancellation.
  // Returns true if a capture was initiated successfully.
 bool StartCapture(const gfx::Rect& bounds,
                   base::TimeDelta delay,
                   base::OnceCallback<void(bool)> completion_callback);

  // Cancels the active watcher. If `success == true` we treat this as a
  // successful completion; if `false` we report failure (e.g., widget closed).
 void StopCapture(bool success);

 private:
  // aura::WindowObserver:
  void OnWindowDestroying(aura::Window* window) override;

  // MouseWatcherHost that monitors an arbitrary rect in `view_` coordinates.
  class RectMouseWatcherHost : public views::MouseWatcherHost {
   public:
    RectMouseWatcherHost(const views::View* view, const gfx::Rect& view_bounds);
    ~RectMouseWatcherHost() override;

    bool Contains(const gfx::Point& screen_point, EventType type) override;

   private:
    bool IsMouseOverWindow() const;

    raw_ptr<const views::View> view_;
    gfx::Rect view_bounds_;
  };

  // Listener for the active watcher so we can signal completion.
  class RequestListener : public views::MouseWatcherListener {
   public:
    explicit RequestListener(MouseEventHandler* owner);
    ~RequestListener() override;

    void MouseMovedOutOfHost() override;

   private:
    raw_ptr<MouseEventHandler> owner_;
  };

  void BeginCapture(gfx::NativeWindow native_window);
  void StopObserving();
  void OnWatcherMovedOut();
  void HandlePointerInside(bool inside);
  void CompleteCapture(bool success);

  raw_ptr<CustomBrowserView> custom_browser_view_;
  raw_ptr<aura::Window> root_window_;
  base::ScopedObservation<aura::Window, aura::WindowObserver>
      root_window_observation_{this};

  gfx::Rect capture_bounds_;
  std::unique_ptr<views::MouseWatcher> watcher_;
  std::unique_ptr<RequestListener> listener_;
  base::OnceCallback<void(bool)> completion_;
  base::OneShotTimer outside_timer_;
  base::TimeDelta max_outside_delay_;
  bool pointer_inside_ = false;
  bool capture_active_ = false;

  base::WeakPtrFactory<MouseEventHandler> weak_ptr_factory_{this};
};

#endif  // CUSTOM_BROWSER_UI_VIEW_MOUSE_EVENT_HANDLER_H_
