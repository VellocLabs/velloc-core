#ifndef CUSTOM_BROWSER_UI_WEBUI_CUSTOM_BROWSER_HANDLER_H_
#define CUSTOM_BROWSER_UI_WEBUI_CUSTOM_BROWSER_HANDLER_H_

#include <map>

#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "chrome/browser/themes/theme_service_observer.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/thumbnails/thumbnail_tracker.h"
#include "chrome/browser/ui/webui/tab_strip/tab_before_unload_tracker.h"
#include "components/saved_tab_groups/public/tab_group_sync_service.h"
#include "components/tabs/public/tab_group.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "custom_browser/ui/mojom/browser_controller.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/native_theme_observer.h"
#include "ui/views/widget/widget_observer.h"

class Browser;
class CustomBrowserView;
namespace tabs {
class TabInterface;
}  // namespace tabs

class CustomBrowserUIEmbedder;

class CustomBrowserHandler : public custom_browser::mojom::BrowserHandler,
                             public TabStripModelObserver,
                             public ThemeServiceObserver,
                             public views::WidgetObserver,
                             public ui::NativeThemeObserver {
 public:
  CustomBrowserHandler(
      mojo::PendingReceiver<custom_browser::mojom::BrowserHandler> receiver,
      mojo::PendingRemote<custom_browser::mojom::Browser> page,
      content::WebUI* web_ui,
      CustomBrowserView* custom_browser_view);
  CustomBrowserHandler(const CustomBrowserHandler&) = delete;
  CustomBrowserHandler& operator=(const CustomBrowserHandler&) = delete;
  ~CustomBrowserHandler() override;

  void NotifyLayoutChanged();
  void NotifyReceivedKeyboardFocus();
  void NotifyContextMenuClosed();

  // TabStripModelObserver:
  void OnTabGroupChanged(const TabGroupChange& change) override;
  void TabGroupedStateChanged(TabStripModel* tab_strip_model,
                              std::optional<tab_groups::TabGroupId> old_group,
                              std::optional<tab_groups::TabGroupId> new_group,
                              tabs::TabInterface* tab,
                              int index) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      int index,
                      TabChangeType change_type) override;
  void OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) override;
  void OnTabBlockedStateChanged(tabs::TabInterface* tab, int index) override;

 private:
  void OnLongPressTimer();
  bool NotifyTabDataChanged(content::WebContents* contents,
                            const tabs::TabInterface* tab,
                            int index);
  custom_browser::mojom::TabPtr GetTabData(content::WebContents* contents,
                                           const tabs::TabInterface* tab,
                                           int index);
  custom_browser::mojom::TabGroupVisualDataPtr GetTabGroupData(TabGroup* group);
  void HandleThumbnailUpdate(content::WebContents* tab,
                             ThumbnailTracker::CompressedThumbnailData image);
  void OnTabCloseCancelledByUser(content::WebContents* tab);
  void ReportTabDurationHistogram(const char* histogram_fragment,
                                  int tab_count,
                                  base::TimeDelta duration);
  gfx::ImageSkia ThemeFavicon(const gfx::ImageSkia& source,
                              bool active_tab_icon);

  // ThemeServiceObserver implementation.
  void OnThemeChanged() override;

  // ui::NativeThemeObserver:
  void OnNativeThemeUpdated(ui::NativeTheme* observed_theme) override;

  // custom_browser::mojom::BrowserHandler:
  void GetTabs(GetTabsCallback callback) override;
  void GetGroupVisualData(GetGroupVisualDataCallback callback) override;
  void CloseContainer() override;
  void CloseTab(int32_t tab_id, bool tab_was_swiped) override;
  void ShowEditDialogForGroup(const std::string& group_id,
                              int32_t location_x,
                              int32_t location_y,
                              int32_t width,
                              int32_t height) override;
  void ShowBackgroundContextMenu(int32_t location_x,
                                 int32_t location_y) override;
  void ShowTabContextMenu(int32_t tab_id,
                          int32_t location_x,
                          int32_t location_y) override;
  void ShowTabFloatingPreview(int32_t tab_id,
                              int32_t location_x,
                              int32_t location_y) override;
  void CloseTabFloatingPreview(int32_t tab_id) override;
  void GetLayout(GetLayoutCallback callback) override;
  void GroupTab(int32_t tab_id, const std::string& group_id) override;
  void UngroupTab(int32_t tab_id) override;
  void MoveGroup(const std::string& group_id, int32_t to_index) override;
  void MoveTab(int32_t tab_id, int32_t to_index) override;
  void SetThumbnailTracked(int32_t tab_id, bool thumbnail_tracked) override;
  void ReportTabActivationDuration(uint32_t duration_ms) override;
  void ReportTabDataReceivedDuration(uint32_t tab_count,
                                     uint32_t duration_ms) override;
  void ReportTabCreationDuration(uint32_t tab_count,
                                 uint32_t duration_ms) override;
  void ActivateTab(int32_t tab_id) override;
  void ExecuteBrowserCommand(
      custom_browser::mojom::BrowserCommandID command_id) override;

  mojo::Receiver<custom_browser::mojom::BrowserHandler> receiver_;
  mojo::Remote<custom_browser::mojom::Browser> page_browser_;

  const raw_ptr<content::WebUI> web_ui_;

  const raw_ptr<CustomBrowserView> custom_browser_view_;
  const raw_ptr<Browser> browser_;

  std::map<content::WebContents*, custom_browser::mojom::TabPtr>
      web_contents_to_tab_data_;

  ThumbnailTracker thumbnail_tracker_;
  tab_strip_ui::TabBeforeUnloadTracker tab_before_unload_tracker_;

  // int guest_last_active_id_;

  base::ScopedObservation<ui::NativeTheme, ui::NativeThemeObserver>
      theme_observation_{this};

  base::WeakPtrFactory<CustomBrowserHandler> weak_ptr_factory_{this};
};

#endif  // CUSTOM_BROWSER_UI_WEBUI_CUSTOM_BROWSER_HANDLER_H_
