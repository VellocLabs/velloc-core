#include "custom_browser/ui/view/nexus_mode_button.h"

#include <optional>
#include <string_view>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/tabs/tab_strip_action_container.h"
#include "custom_browser/common/branding_constants.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/layout/layout_types.h"
#include "ui/views/style/typography.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"

namespace custom_browser {

namespace {

constexpr std::u16string_view kOpenClawEmoji = u"\U0001F99E";
constexpr std::u16string_view kBrowserEmoji = u"\U0001F310";
constexpr std::u16string_view kSwitchToBrowserLabel = u"Switch to Browser";

// Composed at call time so the centralized product name from
// custom_browser/common/branding_constants.h is the single source of truth.
std::u16string SwitchToProductLabel() {
  return base::StrCat(
      {u"Switch to ",
       base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName)});
}
constexpr float kButtonCornerRadius = 10.0f;
constexpr int kButtonTextSizeDelta = 4;

bool ShouldShowNexusModeButton(custom_browser::mojom::UIMode mode) {
  using custom_browser::mojom::UIMode;
  switch (mode) {
    case UIMode::kSidebar:
    case UIMode::kCompactSidebar:
    case UIMode::kStandard:
      return true;
    // kNexus intentionally hidden: the dedicated nexus window does not show the
    // toggle button.
    default:
      return false;
  }
}

TabStripActionContainer* FindTabStripActionContainer(const views::View* root) {
  if (!root) {
    return nullptr;
  }

  if (root->GetClassName() == TabStripActionContainer::kViewClassName) {
    return const_cast<TabStripActionContainer*>(
        static_cast<const TabStripActionContainer*>(root));
  }

  for (const auto& child : root->children()) {
    if (auto* container = FindTabStripActionContainer(child.get())) {
      return container;
    }
  }

  return nullptr;
}

TabStripActionContainer* GetParentTabStripActionContainer(views::View* view) {
  return view && view->parent() &&
                 view->parent()->GetClassName() ==
                     TabStripActionContainer::kViewClassName
             ? static_cast<TabStripActionContainer*>(view->parent())
             : nullptr;
}

}  // namespace

NexusModeButton::NexusModeButton(CustomBrowserView* custom_browser_view)
    : views::LabelButton(PressedCallback(), kOpenClawEmoji),
      custom_browser_view_(custom_browser_view),
      browser_ui_prefs_(custom_browser_view ? custom_browser_view->ui_prefs()
                                            : nullptr) {
  DCHECK(custom_browser_view_);
  DCHECK(browser_ui_prefs_);

  SetCallback(base::BindRepeating(&NexusModeButton::ToggleMode,
                                  base::Unretained(this)));
  SetFocusBehavior(views::View::FocusBehavior::ACCESSIBLE_ONLY);
  SetInstallFocusRingOnFocus(true);
  SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(4, 8)));
  SetMinSize(gfx::Size(32, 28));
  SetHorizontalAlignment(gfx::ALIGN_CENTER);
  SetLabelStyle(views::style::STYLE_BODY_3_EMPHASIS);
  label()->SetFontList(
      label()->font_list().DeriveWithSizeDelta(kButtonTextSizeDelta));
  SetTextSubpixelRenderingEnabled(false);
  SetFocusRingCornerRadius(kButtonCornerRadius);
  SetProperty(views::kCrossAxisAlignmentKey, views::LayoutAlignment::kCenter);

  const custom_browser::mojom::UIMode initial_mode =
      browser_ui_prefs_->current_mode();
  current_ui_mode_ = initial_mode;
  UpdateForMode(initial_mode);
  SetVisible(ShouldShowNexusModeButton(initial_mode));
  UpdateBackground();
}

NexusModeButton::~NexusModeButton() = default;

void NexusModeButton::EnsureAttached() {
  TabStripActionContainer* tab_strip_action_container =
      GetParentTabStripActionContainer(this);
  if (!tab_strip_action_container) {
    tab_strip_action_container = FindTabStripActionContainer(
        custom_browser_view_ ? custom_browser_view_->parent() : nullptr);
  }

  if (!tab_strip_action_container) {
    return;
  }

  if (parent() != tab_strip_action_container) {
    tab_strip_action_container->AddChildViewAt(
        this, tab_strip_action_container->children().size());
  } else {
    const std::optional<size_t> nexus_index =
        tab_strip_action_container->GetIndexOf(this);
    if (nexus_index) {
      const size_t desired_index =
          tab_strip_action_container->children().size() - 1;
      if (*nexus_index != desired_index) {
        tab_strip_action_container->ReorderChildView(this, desired_index);
      }
    }
  }

  if (current_ui_mode_) {
    ApplyModeState(*current_ui_mode_);
  }
}

void NexusModeButton::OnUIModeChanged(custom_browser::mojom::UIMode ui_mode) {
  current_ui_mode_ = ui_mode;
  EnsureAttached();
  ApplyModeState(ui_mode);
}

void NexusModeButton::OnThemeChanged() {
  views::LabelButton::OnThemeChanged();
  SetEnabledTextColors(ui::kColorSysOnSurface);
  SetTextColor(views::Button::STATE_DISABLED, ui::kColorSysOnSurface);
  UpdateBackground();
}

void NexusModeButton::OnFocus() {
  views::LabelButton::OnFocus();
  UpdateBackground();
}

void NexusModeButton::OnBlur() {
  views::LabelButton::OnBlur();
  UpdateBackground();
}

void NexusModeButton::StateChanged(ButtonState old_state) {
  views::LabelButton::StateChanged(old_state);
  UpdateBackground();
}

void NexusModeButton::ToggleMode(const ui::Event& event) {
  (void)event;
  if (!browser_ui_prefs_ || !custom_browser_view_ ||
      !custom_browser_view_->browser() ||
      !custom_browser_view_->browser()->profile()) {
    return;
  }

  using custom_browser::mojom::UIMode;
  Profile* profile = custom_browser_view_->browser()->profile();

  if (browser_ui_prefs_->current_mode() == UIMode::kNexus) {
    // Inside the nexus window: leave toward a normal browser window.
    window_util::OpenOrFocusNormalWindow(profile);
  } else {
    // On a browser window: go to the (singleton) nexus window.
    window_util::OpenOrFocusNexusWindow(profile);
  }
}

void NexusModeButton::ApplyModeState(custom_browser::mojom::UIMode ui_mode) {
  UpdateForMode(ui_mode);

  const bool should_show = ShouldShowNexusModeButton(ui_mode);
  const bool visibility_changed = (GetVisible() != should_show);
  SetVisible(should_show);

  // The button is a FlexLayout child of TabStripActionContainer. When its
  // visibility flips, propagate a preferred-size change so the container (and
  // the tab strip region above it) recomputes its width — reclaiming the space
  // when hidden, making room when shown, instead of leaving a gap.
  if (visibility_changed) {
    PreferredSizeChanged();
  }

  auto* tab_strip_action_container = GetParentTabStripActionContainer(this);
  if (!tab_strip_action_container) {
    return;
  }

  auto* glic_button = tab_strip_action_container->GetGlicButton();
  if (!glic_button) {
    return;
  }

  if (ui_mode == custom_browser::mojom::UIMode::kNexus) {
    if (!glic_button_hidden_for_nexus_) {
      glic_button_visible_before_nexus_ = glic_button->GetVisible();
      tab_strip_action_container->SetGlicShowState(false);
      glic_button_hidden_for_nexus_ = true;
    }
  } else if (glic_button_hidden_for_nexus_) {
    tab_strip_action_container->SetGlicShowState(
        glic_button_visible_before_nexus_);
    glic_button_hidden_for_nexus_ = false;
  }
}

void NexusModeButton::UpdateForMode(custom_browser::mojom::UIMode mode) {
  const bool is_nexus = (mode == custom_browser::mojom::UIMode::kNexus);
  const std::u16string action_label(is_nexus
                                        ? std::u16string(kSwitchToBrowserLabel)
                                        : SwitchToProductLabel());
  SetText(is_nexus ? kBrowserEmoji : kOpenClawEmoji);
  SetTooltipText(action_label);
  GetViewAccessibility().SetName(action_label);
}

void NexusModeButton::UpdateBackground() {
  if (GetState() == views::Button::STATE_PRESSED) {
    SetBackground(views::CreateRoundedRectBackground(ui::kColorSysStatePressed,
                                                     kButtonCornerRadius));
    return;
  }

  if (GetState() == views::Button::STATE_HOVERED || HasFocus()) {
    SetBackground(views::CreateRoundedRectBackground(
        ui::kColorSysStateHoverOnSubtle, kButtonCornerRadius));
    return;
  }

  SetBackground(nullptr);
}

BEGIN_METADATA(NexusModeButton)
END_METADATA

}  // namespace custom_browser
