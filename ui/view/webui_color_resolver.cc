#include "custom_browser/ui/view/webui_color_resolver.h"

#include "chrome/browser/ui/color/chrome_color_id.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
// No state; only uses ColorProvider input.

namespace webui_color_resolver {

std::optional<SkColor> ResolveChromeColorId(
    custom_browser::mojom::ChromeColorID id,
    const ui::ColorProvider* color_provider) {
  if (!color_provider) {
    return std::nullopt;
  }
  switch (id) {
    case custom_browser::mojom::ChromeColorID::kColorWebUiTabStripBackground:
      return color_provider->GetColor(ui::kColorSysHeader);
    case custom_browser::mojom::ChromeColorID::
        kColorToolbarBackgroundSubtleEmphasis:
      return color_provider->GetColor(kColorToolbarBackgroundSubtleEmphasis);
    default:
      break;
  }
  return std::nullopt;
}

std::optional<ui::ColorId> ResolveChromeColorId(
    custom_browser::mojom::ChromeColorID id) {
  switch (id) {
    case custom_browser::mojom::ChromeColorID::kColorWebUiTabStripBackground:
      return ui::kColorSysHeader;
    case custom_browser::mojom::ChromeColorID::
        kColorToolbarBackgroundSubtleEmphasis:
      return kColorToolbarBackgroundSubtleEmphasis;
    default:
      break;
  }
  return std::nullopt;
}

}  // namespace webui_color_resolver
