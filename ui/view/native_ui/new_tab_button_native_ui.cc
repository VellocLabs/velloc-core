#include "custom_browser/ui/view/native_ui/new_tab_button_native_ui.h"

#include <memory>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/grit/generated_resources.h"
#include "components/vector_icons/vector_icons.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/event_constants.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"

namespace custom_browser::native_ui {

NewTabButtonNativeUI::NewTabButtonNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* params)
    : NativeUI(host_view, params) {}

NewTabButtonNativeUI::~NewTabButtonNativeUI() = default;

void NewTabButtonNativeUI::OnPrepare() {
  TabStrip* tab_strip =
      GetCustomBrowserView()->horizontal_tab_strip_for_testing();
  if (!tab_strip) {
    return;
  }

  auto new_tab_button = std::make_unique<ToolbarButton>(base::BindRepeating(
      &TabStrip::NewTabButtonPressed, base::Unretained(tab_strip)));
  new_tab_button->SetVectorIcon(vector_icons::kAddOldIcon);
  new_tab_button->SetTooltipText(
      l10n_util::GetStringUTF16(IDS_TOOLTIP_NEW_TAB));
  new_tab_button->GetViewAccessibility().SetName(
      l10n_util::GetStringUTF16(IDS_ACCNAME_NEWTAB));
#if BUILDFLAG(IS_LINUX)
  new_tab_button->SetTriggerableEventFlags(
      new_tab_button->GetTriggerableEventFlags() | ui::EF_MIDDLE_MOUSE_BUTTON);
#endif

  ToolbarButton* button = host_view()->AddChildView(std::move(new_tab_button));
  AddView(button);
}

void NewTabButtonNativeUI::OnLayout() {
  auto* button = GetNewTabButton();
  if (!button) {
    return;
  }
  UpdateViewsVisibility();
  if (visible()) {
    button->SetBoundsRect(bounds_);
  }
}

ToolbarButton* NewTabButtonNativeUI::GetNewTabButton() const {
  return static_cast<ToolbarButton*>(view());
}

}  // namespace custom_browser::native_ui
