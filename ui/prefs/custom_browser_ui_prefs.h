// A lightweight helper to persist and restore Custom Browser UI settings
// (starting with native child layout params) into Profile prefs.
//
// This keeps NativeUIContainerView focused on view bookkeeping and delegates
// serialization and storage concerns here so we can extend to more UI
// settings in the future.
#ifndef CUSTOM_BROWSER_UI_PREFS_CUSTOM_BROWSER_UI_PREFS_H_
#define CUSTOM_BROWSER_UI_PREFS_CUSTOM_BROWSER_UI_PREFS_H_

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/values.h"
#include "components/prefs/pref_change_registrar.h"
#include "custom_browser/ui/mojom/browser_constants.mojom.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"

class PrefService;
class PrefRegistrySimple;
class Profile;
namespace content {
class WebUIDataSource;
}  // namespace content
class GURL;
using custom_browser::mojom::BoolKey;
using custom_browser::mojom::IntKey;
using custom_browser::mojom::NativeViewParams;
using custom_browser::mojom::NativeViewParamsPtr;
using custom_browser::mojom::UIMode;

class CustomBrowserUiModePrefPaths;

// Centralizes persistence of Custom Browser UI preferences.
class CustomBrowserUiPrefs {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnUIModeChanged(UIMode ui_mode) {}
    virtual void OnPrefUIModeChanged(UIMode ui_mode) {}
    virtual void OnLayoutUpdated(const NativeViewParams* old_params,
                                 const NativeViewParams* new_params) {}
    virtual void OnLayoutAdded(const NativeViewParams* layout) {}
    virtual void OnLayoutRemoved(const NativeViewParams* layout) {}

   protected:
    ~Observer() override = default;
  };

  void AddObserver(Observer* obs);
  void RemoveObserver(Observer* obs);
  // Registers the dictionary preference for profile prefs.
  static void RegisterProfilePrefs(PrefRegistrySimple* registry);

  explicit CustomBrowserUiPrefs(PrefService* prefs);
  CustomBrowserUiPrefs(const CustomBrowserUiPrefs&) = delete;
  CustomBrowserUiPrefs& operator=(const CustomBrowserUiPrefs&) = delete;
  ~CustomBrowserUiPrefs();

  static void InitMainUI(content::WebUIDataSource* source, PrefService* prefs);
  static UIMode GetUIModeFromPref(PrefService* prefs);
  // True for the modes a single window is pinned to (kNexus, kSingleWeb):
  // never persisted as the current mode, never following the shared pref.
  static bool IsPerWindowMode(UIMode mode);

  // Forces the mode loaded by the next BuildInitUrl()/RestoreFromPrefs() call,
  // overriding pref_mode(). Used to open a window directly in kNexus without
  // touching the persisted current-mode pref.
  void set_initial_mode_override(std::optional<UIMode> mode) {
    initial_mode_override_ = mode;
  }

  GURL BuildInitUrl();
  // Loads any saved child layouts and applies them to the given manager.
  // Uses a temporary restoring flag so NativeUIContainerView can avoid
  // re-saving while restoration is in progress.
  void RestoreFromPrefs();

  // Persists (upserts) layout entries.
  void UpdateLayouts(std::vector<NativeViewParamsPtr> batch);

  void UpdateUIMode(UIMode mode);
  void SaveIntPref(IntKey key, int32_t value);
  void SaveBoolPref(BoolKey key, bool value);
  void EnableLayoutsSaving(bool enable);

  bool IsValidLayout(const NativeViewParams* layout) const;
  UIMode pref_mode() const;
  UIMode current_mode() const;
  bool IsChromiumUI() const { return current_mode() == UIMode::kStandard; }
  void OnBrowserClosing();

 private:
  void NotifyAllLayouts(bool added);
  void UpdateLayout(NativeViewParamsPtr* existing, NativeViewParamsPtr& update);
  void AddLayout(NativeViewParamsPtr& layout);

  void SavePrefImpl(const std::string& key, base::Value value, bool global);

  bool LoadUILayoutParams(const UIMode& mode);
  void SaveCurrentUILayoutParams();
  NativeViewParamsPtr* FindExistingLayout(const NativeViewParamsPtr& layout);
  void OnModePrefChanged();
  void OnNewTabPrefChanged();

  raw_ptr<PrefService> prefs_;
  custom_browser::mojom::UILayoutParamsPtr ui_layout_params_ptr_;
  std::unordered_set<int32_t> touched_hash_ids_;
  bool enable_layouts_saving_ = true;
  std::optional<UIMode> initial_mode_override_;

  // Browser-mode default until LoadUILayoutParams() sets the real mode. Keeps
  // current_mode() safe to read before init (e.g. window-mode lookups).
  UIMode ui_mode_ = UIMode::kSidebar;
  base::ObserverList<Observer> observers_;
  std::unique_ptr<CustomBrowserUiModePrefPaths> mode_pref_paths_;
  PrefChangeRegistrar pref_change_registrar_;
  base::WeakPtrFactory<CustomBrowserUiPrefs> weak_ptr_factory_{this};
};

#endif  // CUSTOM_BROWSER_UI_PREFS_CUSTOM_BROWSER_UI_PREFS_H_
