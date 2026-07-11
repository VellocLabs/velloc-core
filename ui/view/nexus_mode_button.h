#ifndef CUSTOM_BROWSER_UI_VIEW_NEXUS_MODE_BUTTON_H_
#define CUSTOM_BROWSER_UI_VIEW_NEXUS_MODE_BUTTON_H_

#include <optional>

#include "base/memory/raw_ptr.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom-forward.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/button/label_button.h"

class CustomBrowserUiPrefs;
class CustomBrowserView;

namespace ui {
class Event;
}  // namespace ui

namespace custom_browser {

class NexusModeButton : public views::LabelButton {
  METADATA_HEADER(NexusModeButton, views::LabelButton)

 public:
  explicit NexusModeButton(CustomBrowserView* custom_browser_view);
  ~NexusModeButton() override;

  NexusModeButton(const NexusModeButton&) = delete;
  NexusModeButton& operator=(const NexusModeButton&) = delete;

  void EnsureAttached();
  void OnUIModeChanged(custom_browser::mojom::UIMode ui_mode);

  // views::LabelButton:
  void OnThemeChanged() override;
  void OnFocus() override;
  void OnBlur() override;
  void StateChanged(ButtonState old_state) override;

 private:
  void ToggleMode(const ui::Event& event);
  void ApplyModeState(custom_browser::mojom::UIMode ui_mode);
  void UpdateForMode(custom_browser::mojom::UIMode mode);
  void UpdateBackground();

  raw_ptr<CustomBrowserView> custom_browser_view_;
  raw_ptr<CustomBrowserUiPrefs> browser_ui_prefs_;
  std::optional<custom_browser::mojom::UIMode> current_ui_mode_;
  bool glic_button_visible_before_nexus_ = false;
  bool glic_button_hidden_for_nexus_ = false;
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_NEXUS_MODE_BUTTON_H_
