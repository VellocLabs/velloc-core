#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_UTILS_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_UTILS_H_

#include <string>

class ToolbarButton;
class BrowserView;
class CustomBrowserView;
namespace views {
class View;
}  // namespace views

namespace custom_browser::native_ui {
class NativeUI;
}  // namespace custom_browser::native_ui

namespace custom_browser::native_ui::utils {

void PrintViewHierarchyLog(CustomBrowserView* browser_view);

bool SaveChromiumHierarchy(CustomBrowserView* browser_view);
bool RestoreChromiumHierarchy(CustomBrowserView* browser_view);

void StartObservingViewLayout(NativeUI* native_ui, views::View* view);
void StopObservingViewLayout(views::View* view);

void SaveDownloadButton(views::View* browser_view,
                        ToolbarButton* download_button);
// download ui controller uses this to get the download button.
ToolbarButton* GetDownloadsButton(BrowserView* browser_view);
bool IsChromiumUI(BrowserView* browser_view);

std::unique_ptr<views::View> CreateDividerView(bool use_dark_color = false);
views::View* FindUnusedView(views::View* parent_view,
                            const std::string& view_class_name);
void MarkViewUsed(views::View* view, const std::string& view_class_name);
void ReleaseView(views::View* view);

views::View* FindViewByClassInHierarchy(views::View* root,
                                        const std::string& class_name);

}  // namespace custom_browser::native_ui::utils

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_UTILS_H_
