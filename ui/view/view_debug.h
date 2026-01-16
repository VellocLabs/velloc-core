#ifndef CUSTOM_BROWSER_UI_VIEW_VIEW_DEBUG_H_
#define CUSTOM_BROWSER_UI_VIEW_VIEW_DEBUG_H_

#include "base/logging.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/gfx/geometry/size.h"
#include "ui/base/metadata/metadata_header_macros.h"

class ViewDebug : public views::ViewObserver{
 public:
  ViewDebug(views::View* view, const std::string& msg = "") : msg_(msg) {
    view->AddObserver(this);
    LOG(INFO) << msg_;
    LOG(INFO) << "Start Tracking view: " << (view ? view->GetClassName() : "null");
  }
  ~ViewDebug() override = default;

  static void Track(views::View* view, const std::string& msg) {
    new ViewDebug(view, msg);
  }

  void OnViewVisibilityChanged(views::View* observed_view,
                               views::View* starting_view,
                               bool visible) override;
 private:
  void OnViewIsDeleting(views::View* observed_view) override;
  std::string msg_;
};

#endif  // CUSTOM_BROWSER_UI_VIEW_VIEW_DEBUG_H_
