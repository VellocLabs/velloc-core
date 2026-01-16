#ifndef CUSTOM_BROWSER_UI_VIEW_WEBUI_COLOR_RESOLVER_H_
#define CUSTOM_BROWSER_UI_VIEW_WEBUI_COLOR_RESOLVER_H_

#include <optional>

#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/color/color_id.h"


namespace ui {
class ColorProvider;
}

// Namespace of helpers for mapping WebUI color identifiers to SkColor.
namespace webui_color_resolver {

// Resolve a ChromeColorID enum to SkColor using the provided ColorProvider.
std::optional<SkColor> ResolveChromeColorId(
    custom_browser::mojom::ChromeColorID id,
    const ui::ColorProvider* color_provider);

// New: Resolve a ChromeColorID enum to a ui::ColorId constant without
// requiring a ColorProvider. Callers can then fetch the actual color later
// (e.g., on theme changes) via ColorProvider::GetColor(id).
std::optional<ui::ColorId> ResolveChromeColorId(
    custom_browser::mojom::ChromeColorID id);

}  // namespace webui_color_resolver

#endif  // CUSTOM_BROWSER_UI_VIEW_WEBUI_COLOR_RESOLVER_H_
