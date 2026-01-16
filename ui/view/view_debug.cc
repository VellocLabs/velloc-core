#include "custom_browser/ui/view/view_debug.h"

void ViewDebug::OnViewVisibilityChanged(views::View* observed_view,
                                        views::View* /*starting_view*/,
                                        bool visible) {
  LOG(INFO) << msg_;
  LOG(INFO) << "OnViewVisibilityChanged: "
            << (observed_view ? observed_view->GetClassName() : "null")
            << " visible=" << (visible ? "true" : "false");
}

void ViewDebug::OnViewIsDeleting(views::View* observed_view) {
  LOG(INFO) << msg_;
  LOG(INFO) << "OnViewIsDeleting: "
            << (observed_view ? observed_view->GetClassName() : "null");
  if (observed_view)
    observed_view->RemoveObserver(this);
  delete this;
}

