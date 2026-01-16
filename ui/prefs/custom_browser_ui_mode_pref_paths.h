#ifndef CUSTOM_BROWSER_UI_PREFS_CUSTOM_BROWSER_UI_MODE_PREF_PATHS_H_
#define CUSTOM_BROWSER_UI_PREFS_CUSTOM_BROWSER_UI_MODE_PREF_PATHS_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "components/prefs/pref_change_registrar.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"


class PrefRegistrySimple;
class PrefService;

class CustomBrowserUiModePrefPaths : public CustomBrowserUiPrefs::Observer {
 public:
  static void RegisterProfilePrefs(PrefRegistrySimple* registry);

  CustomBrowserUiModePrefPaths(PrefService* prefs, CustomBrowserUiPrefs* owner);
  CustomBrowserUiModePrefPaths(const CustomBrowserUiModePrefPaths&) = delete;
  CustomBrowserUiModePrefPaths& operator=(const CustomBrowserUiModePrefPaths&) =
      delete;
  ~CustomBrowserUiModePrefPaths() override;

  void OnUIModeChanged(UIMode ui_mode) override;

 private:
  void Initialize();
  void OnTrackedPrefChanged(std::string path);
  void PersistPrefForMode(UIMode mode, std::string_view path);
  void RestoreTrackedPrefs(UIMode mode);
  void ApplyValueToPref(std::string_view path, const base::Value* value) const;
  static std::string ModeKey(UIMode mode);

  PrefChangeRegistrar registrar_;
  raw_ptr<PrefService> prefs_;
  raw_ptr<CustomBrowserUiPrefs> owner_;
  UIMode current_mode_;
  bool is_restoring_ = false;
  std::vector<std::string> tracked_pref_paths_;
};

#endif  // CUSTOM_BROWSER_UI_PREFS_CUSTOM_BROWSER_UI_MODE_PREF_PATHS_H_
