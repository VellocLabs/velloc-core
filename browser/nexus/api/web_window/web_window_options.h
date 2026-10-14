// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_OPTIONS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_OPTIONS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ui/base/mojom/window_show_state.mojom-shared.h"
#include "ui/gfx/geometry/rect.h"

class GURL;

namespace nexus {

// Pure input checks for the `webWindow` capability (F-29); no browser state.

inline constexpr int kWebWindowMinWidth = 200;
inline constexpr int kWebWindowMinHeight = 150;

// Security: a single-web window shows http, https and file pages only. Every
// other scheme (chrome:, devtools:, javascript:, data:, about: …) is refused
// before anything is created or navigated.
bool IsAllowedWebWindowUrl(const GURL& url);

// 'normal' | 'maximized' | 'minimized' → the show state; anything else is
// nullopt.
std::optional<ui::mojom::WindowShowState> ParseWebWindowState(
    std::string_view state);

// The wire name of a window's state, from its two flags.
std::string_view WebWindowStateName(bool maximized, bool minimized);

// Requested geometry: each field is optional on the wire.
struct WebWindowBoundsRequest {
  std::optional<int32_t> left;
  std::optional<int32_t> top;
  std::optional<int32_t> width;
  std::optional<int32_t> height;
};

// Applies `request` over `current`: an absent field keeps its value. Returns
// the error reason (for `invalid_args`) when a size is below the minimum.
std::optional<std::string> ResolveWebWindowBounds(
    const WebWindowBoundsRequest& request,
    const gfx::Rect& current,
    gfx::Rect& out);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_OPTIONS_H_
