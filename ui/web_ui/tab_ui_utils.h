#ifndef CUSTOM_BROWSER_UI_WEB_UI_TAB_UI_UTILS_H_
#define CUSTOM_BROWSER_UI_WEB_UI_TAB_UI_UTILS_H_

#include <cstdint>

#include "base/memory/raw_ptr.h"
#include "ui/gfx/geometry/point.h"

class Browser;
class BrowserView;
class Profile;
class Tab;
class TabStrip;

namespace views {
class View;
}

namespace custom_browser {

namespace TabUIUtils {

enum class TabLookupResult {
  kSuccess,
  kNotFound,
  kDifferentBrowser,
};

struct TabContext {
  raw_ptr<Browser> browser = nullptr;
  raw_ptr<TabStrip> tab_strip = nullptr;
  raw_ptr<Tab> tab = nullptr;
  gfx::Point target_screen_point;
  int tab_index = -1;
  TabLookupResult lookup_result = TabLookupResult::kNotFound;
};

TabContext PopulateTabContext(Browser* expected_browser,
                              int32_t tab_id,
                              int32_t location_x,
                              int32_t location_y);

bool ShowTabContextMenuWithController(const TabContext& context);

bool ShowTabFloatingPreview(const TabContext& context);

bool CloseTabFloatingPreview(const TabContext& context);

}  // namespace TabUIUtils

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_WEB_UI_TAB_UI_UTILS_H_
