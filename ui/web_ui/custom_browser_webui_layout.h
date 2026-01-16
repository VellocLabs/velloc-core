#ifndef CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_LAYOUT_H_
#define CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_LAYOUT_H_

#include "base/containers/flat_map.h"
#include "ui/gfx/geometry/size.h"

struct CustomBrowserWebUILayout {
  static CustomBrowserWebUILayout CalculateForWebViewportSize(
      const gfx::Size& viewport_size);

  // Returns the tab strip's total height. This should be used to size
  // its container.
  static int GetContainerHeight();

  // Returns a dictionary of CSS variables.
  base::flat_map<std::string, std::string> AsDictionary() const;

  int viewport_width = 0;
  gfx::Size tab_thumbnail_size;
  double tab_thumbnail_aspect_ratio = 1.0;
};

#endif  // CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_LAYOUT_H_
