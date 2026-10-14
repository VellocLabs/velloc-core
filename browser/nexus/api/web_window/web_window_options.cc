// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_options.h"

#include "url/gurl.h"
#include "url/url_constants.h"

namespace nexus {

bool IsAllowedWebWindowUrl(const GURL& url) {
  return url.is_valid() &&
         (url.SchemeIs(url::kHttpScheme) || url.SchemeIs(url::kHttpsScheme) ||
          url.SchemeIs(url::kFileScheme));
}

std::optional<ui::mojom::WindowShowState> ParseWebWindowState(
    std::string_view state) {
  if (state == "normal") {
    return ui::mojom::WindowShowState::kNormal;
  }
  if (state == "maximized") {
    return ui::mojom::WindowShowState::kMaximized;
  }
  if (state == "minimized") {
    return ui::mojom::WindowShowState::kMinimized;
  }
  return std::nullopt;
}

std::string_view WebWindowStateName(bool maximized, bool minimized) {
  if (minimized) {
    return "minimized";
  }
  return maximized ? "maximized" : "normal";
}

std::optional<std::string> ResolveWebWindowBounds(
    const WebWindowBoundsRequest& request,
    const gfx::Rect& current,
    gfx::Rect& out) {
  const int width = request.width.value_or(current.width());
  const int height = request.height.value_or(current.height());
  if (width < kWebWindowMinWidth) {
    return "width is below the minimum";
  }
  if (height < kWebWindowMinHeight) {
    return "height is below the minimum";
  }
  out = gfx::Rect(request.left.value_or(current.x()),
                  request.top.value_or(current.y()), width, height);
  return std::nullopt;
}

}  // namespace nexus
