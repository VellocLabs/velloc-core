#include "custom_browser/ui/prefs/custom_browser_ui_mode_pref_paths.h"

#include <iterator>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/toolbar/toolbar_pref_names.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"

namespace {

inline constexpr char kCustomBrowserUiModePrefRoot[] = "custom_browser.mapping";
constexpr const char* kTrackedPrefPaths[] = {
    prefs::kShowHomeButton,
    prefs::kShowForwardButton,
    prefs::kPinSplitTabButton,
    prefs::kPinnedActions,
};

}  // namespace

// static
void CustomBrowserUiModePrefPaths::RegisterProfilePrefs(
    PrefRegistrySimple* registry) {
  CHECK(registry);
  registry->RegisterDictionaryPref(kCustomBrowserUiModePrefRoot);
}

CustomBrowserUiModePrefPaths::CustomBrowserUiModePrefPaths(
    PrefService* prefs,
    CustomBrowserUiPrefs* owner)
    : prefs_(prefs), owner_(owner), current_mode_(UIMode::kSidebar) {
  DCHECK(prefs_);
  DCHECK(owner_);
  registrar_.Init(prefs_);
  owner_->AddObserver(this);
  Initialize();
}

CustomBrowserUiModePrefPaths::~CustomBrowserUiModePrefPaths() {
  if (owner_) {
    owner_->RemoveObserver(this);
  }
}

void CustomBrowserUiModePrefPaths::Initialize() {
  tracked_pref_paths_.assign(std::begin(kTrackedPrefPaths),
                             std::end(kTrackedPrefPaths));
  for (const std::string& path : tracked_pref_paths_) {
    registrar_.Add(
        path,
        base::BindRepeating(&CustomBrowserUiModePrefPaths::OnTrackedPrefChanged,
                            base::Unretained(this), path));
  }

  // if (tabs::IsVerticalTabsFeatureEnabled()) {
  //   if (BrowserList* browser_list = BrowserList::GetInstance()) {
  //     for (Browser* browser : *browser_list) {
  //       if (auto* controller =
  //               tabs::VerticalTabStripStateController::From(browser)) {
  //         controller->SetVerticalTabsEnabled(true);
  //       }
  //     }
  //   }
  // }
}

void CustomBrowserUiModePrefPaths::OnTrackedPrefChanged(std::string path) {
  if (is_restoring_) {
    return;
  }

  PersistPrefForMode(current_mode_, path);
}

void CustomBrowserUiModePrefPaths::PersistPrefForMode(UIMode mode,
                                                      std::string_view path) {
  const PrefService::Preference* pref = prefs_->FindPreference(path);
  if (!pref) {
    return;
  }

  const base::Value* value = pref->GetValue();
  if (!value) {
    return;
  }

  ScopedDictPrefUpdate update(prefs_, kCustomBrowserUiModePrefRoot);
  base::DictValue& root = update.Get();
  root.EnsureDict(ModeKey(mode))->Set(path, value->Clone());
}

void CustomBrowserUiModePrefPaths::RestoreTrackedPrefs(UIMode mode) {
  const base::DictValue& root = prefs_->GetDict(kCustomBrowserUiModePrefRoot);
  const base::DictValue* stored = root.FindDict(ModeKey(mode));

  base::AutoReset<bool> auto_reset(&is_restoring_, true);
  for (const std::string& path : tracked_pref_paths_) {
    const base::Value* value = stored ? stored->Find(path) : nullptr;
    ApplyValueToPref(path, value);  // Reset to default if missing.
  }
}

void CustomBrowserUiModePrefPaths::ApplyValueToPref(
    std::string_view path,
    const base::Value* value) const {
  const PrefService::Preference* pref = prefs_->FindPreference(path);
  if (!pref) {
    return;
  }

  if (!value) {
    // No stored value: reset to default.
    prefs_->ClearPref(path);
    return;
  }
  prefs_->Set(path, *value);
}

std::string CustomBrowserUiModePrefPaths::ModeKey(UIMode mode) {
  return base::NumberToString(static_cast<int>(mode));
}

void CustomBrowserUiModePrefPaths::OnUIModeChanged(UIMode ui_mode) {
  if (ui_mode == current_mode_) {
    return;
  }
  current_mode_ = ui_mode;
  RestoreTrackedPrefs(current_mode_);
}
