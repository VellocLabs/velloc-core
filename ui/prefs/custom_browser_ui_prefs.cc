#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"

#include <algorithm>
#include <sstream>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/web_ui_data_source.h"
#include "custom_browser/common/pref_names.h"
#include "custom_browser/common/url_constants.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/prefs/custom_browser_ui_mode_pref_paths.h"
#include "custom_browser/ui/view/native_ui_container_view.h"

// For base64 encode/decode of serialized bytes
#include "base/base64.h"
// For mojo struct serialization to bytes
#include "mojo/public/cpp/bindings/lib/serialization.h"
#include "mojo/public/cpp/bindings/type_converter.h"
#include "url/gurl.h"

namespace {

template <typename EnumType>
std::string MojomToString(EnumType value) {
  std::ostringstream oss;
  oss << value;
  return oss.str();
}

template <typename KeyType>
bool IsGlobalKey(KeyType key) {
  return static_cast<int>(key) < static_cast<int>(KeyType::kGlobalEnd);
}

bool SameNativeView(const NativeViewParams& a, const NativeViewParams& b) {
  DCHECK(a.hash_id != 0 && b.hash_id != 0);
  return (a.hash_id == b.hash_id);
}

}  // namespace

// static
void CustomBrowserUiPrefs::RegisterProfilePrefs(PrefRegistrySimple* registry) {
  DCHECK(registry);
  CustomBrowserUiModePrefPaths::RegisterProfilePrefs(registry);
  registry->RegisterDictionaryPref(kCustomChildLayoutsPrefPath);
  registry->RegisterDictionaryPref(kCustomBrowserPrefPath);
  registry->RegisterDictionaryPref(kCustomComponentExtensionsPrefPath);
  // Default WebUI page mode.
  registry->RegisterIntegerPref(kCustomUIModePrefPath,
                                static_cast<int>(UIMode::kSidebar));
  registry->RegisterBooleanPref(kCustomNewTabShowSearchPrefPath, true);
  registry->RegisterBooleanPref(kCustomNewTabShowLogoPrefPath, true);
  registry->RegisterBooleanPref(kCustomNewTabShowShortcutsPrefPath, true);
}

CustomBrowserUiPrefs::CustomBrowserUiPrefs(PrefService* prefs)
    : prefs_(prefs),
      ui_layout_params_ptr_(custom_browser::mojom::UILayoutParams::New()) {
  DCHECK(prefs_);
  mode_pref_paths_ =
      std::make_unique<CustomBrowserUiModePrefPaths>(prefs_, this);
  pref_change_registrar_.Init(prefs_);
  pref_change_registrar_.Add(
      kCustomUIModePrefPath,
      base::BindRepeating(&CustomBrowserUiPrefs::OnModePrefChanged,
                          base::Unretained(this)));
  pref_change_registrar_.Add(
      kCustomNewTabShowSearchPrefPath,
      base::BindRepeating(&CustomBrowserUiPrefs::OnNewTabPrefChanged,
                          base::Unretained(this)));
  pref_change_registrar_.Add(
      kCustomNewTabShowLogoPrefPath,
      base::BindRepeating(&CustomBrowserUiPrefs::OnNewTabPrefChanged,
                          base::Unretained(this)));
  pref_change_registrar_.Add(
      kCustomNewTabShowShortcutsPrefPath,
      base::BindRepeating(&CustomBrowserUiPrefs::OnNewTabPrefChanged,
                          base::Unretained(this)));
}

CustomBrowserUiPrefs::~CustomBrowserUiPrefs() = default;

// static
void CustomBrowserUiPrefs::InitMainUI(content::WebUIDataSource* source,
                                      PrefService* prefs) {
  DCHECK(source);
  DCHECK(prefs);
  const base::DictValue& root = prefs->GetDict(kCustomBrowserPrefPath);

  // 1) Global prefs.
  for (const auto [key, value] : root) {
    // Just to iterate.
    if (value.is_bool()) {
      source->AddBoolean(key, value.GetBool());
    } else if (value.is_int()) {
      source->AddInteger(key, value.GetInt());
    } else if (value.is_string()) {
      source->AddString(key, value.GetString());
    }
  }

  // 2) Load per-UIMode child prefs for the current mode.
  UIMode mode = GetUIModeFromPref(prefs);
  const base::DictValue* prefs_by_mode = root.FindDict(MojomToString(mode));
  if (!prefs_by_mode) {
    return;
  }

  for (const auto [key, value] : *prefs_by_mode) {
    // Just to iterate.
    if (value.is_bool()) {
      source->AddBoolean(key, value.GetBool());
    } else if (value.is_int()) {
      source->AddInteger(key, value.GetInt());
    } else if (value.is_string()) {
      source->AddString(key, value.GetString());
    }
  }
}

void CustomBrowserUiPrefs::SavePrefImpl(const std::string& key,
                                        base::Value value,
                                        bool global) {
  ScopedDictPrefUpdate update(prefs_, kCustomBrowserPrefPath);
  base::DictValue* dict = &update.Get();
  if (!global) {
    dict = dict->EnsureDict(MojomToString(current_mode()));
  }
  dict->Set(key, std::move(value));
}

void CustomBrowserUiPrefs::SaveIntPref(IntKey key, int32_t value) {
  SavePrefImpl(MojomToString(key), base::Value(value), IsGlobalKey(key));
}

void CustomBrowserUiPrefs::SaveBoolPref(BoolKey key, bool value) {
  SavePrefImpl(MojomToString(key), base::Value(value), IsGlobalKey(key));
}

void CustomBrowserUiPrefs::EnableLayoutsSaving(bool enable) {
  if (!enable) {
    SaveCurrentUILayoutParams();
  }
  enable_layouts_saving_ = enable;
}

// static
UIMode CustomBrowserUiPrefs::GetUIModeFromPref(PrefService* prefs) {
  return static_cast<UIMode>(prefs->GetInteger(kCustomUIModePrefPath));
}

UIMode CustomBrowserUiPrefs::pref_mode() const {
  return GetUIModeFromPref(prefs_);
}

UIMode CustomBrowserUiPrefs::current_mode() const {
  return ui_mode_;
}

void CustomBrowserUiPrefs::RestoreFromPrefs() {
  LoadUILayoutParams(pref_mode());
}

GURL CustomBrowserUiPrefs::BuildInitUrl() {
  LoadUILayoutParams(pref_mode());
  return GURL(custom_browser::kChromeUICustomBrowserURL);
}

bool CustomBrowserUiPrefs::LoadUILayoutParams(const UIMode& mode) {
  NotifyAllLayouts(false);
  // clear existing state.
  ui_layout_params_ptr_ = custom_browser::mojom::UILayoutParams::New();
  touched_hash_ids_.clear();
  ui_mode_ = mode;
  const std::string mode_key = MojomToString(mode);
  const base::DictValue& stored =
      prefs_->GetDict(kCustomChildLayoutsPrefPath);
  const std::string* encoded = stored.FindString(mode_key);
  if (encoded) {
    if (auto maybe_bytes = base::Base64Decode(*encoded)) {
      custom_browser::mojom::UILayoutParamsPtr decoded;
      if (custom_browser::mojom::UILayoutParams::Deserialize(*maybe_bytes,
                                                             &decoded) &&
          decoded) {
        if (decoded->version != custom_browser::mojom::kUIVersion) {
          LOG(WARNING) << "Ignoring Custom Browser UI prefs for mode "
                       << mode_key << " due to version mismatch. Expected "
                       << custom_browser::mojom::kUIVersion << " but got "
                       << decoded->version;
        } else {
          ui_layout_params_ptr_ = std::move(decoded);
          DCHECK(ui_mode_ == mode);
        }
      }
    }
  }

  if (mode == UIMode::kStandard) {
    // Chromium mode has no stored prefs.
    DCHECK(ui_layout_params_ptr_->child_layouts.empty());
  }

  ui_layout_params_ptr_->version = custom_browser::mojom::kUIVersion;
  for (auto& obs : observers_) {
    obs.OnUIModeChanged(ui_mode_);
  }
  NotifyAllLayouts(true);
  return true;
}

void CustomBrowserUiPrefs::NotifyAllLayouts(bool added) {
  std::vector<NativeViewParams*> layouts;
  layouts.reserve(ui_layout_params_ptr_->child_layouts.size());
  for (const auto& layout : ui_layout_params_ptr_->child_layouts) {
    if (layout) {
      layouts.push_back(layout.get());
    }
  }

  for (const auto& layout : layouts) {
    if (added) {
      for (auto& obs : observers_) {
        obs.OnLayoutAdded(layout);
      }
    } else {
      for (auto& obs : observers_) {
        obs.OnLayoutRemoved(layout);
      }
    }
  }
}

void CustomBrowserUiPrefs::UpdateLayout(NativeViewParamsPtr* existing,
                                        NativeViewParamsPtr& update) {
  auto* old_ptr = existing->get();
  //*existing = std::move(update);
  for (auto& objs : observers_) {
    objs.OnLayoutUpdated(old_ptr, (&update)->get());
  }
  *existing = std::move(update);
}

void CustomBrowserUiPrefs::AddLayout(NativeViewParamsPtr& layout) {
  for (auto& objs : observers_) {
    objs.OnLayoutAdded(layout.get());
  }
  ui_layout_params_ptr_->child_layouts.push_back(std::move(layout));
}

void CustomBrowserUiPrefs::OnBrowserClosing() {
  SaveCurrentUILayoutParams();
}

void CustomBrowserUiPrefs::SaveCurrentUILayoutParams() {
  if (!enable_layouts_saving_) {
    return;
  }
  if (ui_mode_ == UIMode::kStandard) {
    return;
  }

  // filter ui_layout_params_ptr_ to only include touched layouts
  auto save_layouts = custom_browser::mojom::UILayoutParams::New();
  save_layouts->version = ui_layout_params_ptr_->version;
  for (auto& obs : ui_layout_params_ptr_->child_layouts) {
    if (touched_hash_ids_.find(obs.get()->hash_id) == touched_hash_ids_.end()) {
      continue;
    }
    save_layouts->child_layouts.push_back(obs.Clone());
  }
  // ui_layout_params_ptr_ = std::move(save_layouts);

  // Generated helper: value -> bytes. Persist even when child_layouts is empty
  // so stale data is not left behind for this mode.
  std::vector<uint8_t> bytes =
      custom_browser::mojom::UILayoutParams::Serialize(&save_layouts);
  // Bytes -> Base64 (legacy 2-arg API).
  std::string encoded = base::Base64Encode(bytes);
  // Store under per-mode key.
  ScopedDictPrefUpdate update(prefs_, kCustomChildLayoutsPrefPath);
  base::DictValue& root = update.Get();
  const std::string mode_key = MojomToString(ui_mode_);
  root.Set(mode_key, encoded);
}

void CustomBrowserUiPrefs::UpdateUIMode(UIMode mode) {
  if (ui_mode_ == mode) {
    return;
  }
  SaveCurrentUILayoutParams();
  LoadUILayoutParams(mode);
}

void CustomBrowserUiPrefs::OnModePrefChanged() {
  if (pref_mode() == current_mode()) {
    return;
  }

  for (auto& obs : observers_) {
    obs.OnPrefUIModeChanged(pref_mode());
  }
}

void CustomBrowserUiPrefs::OnNewTabPrefChanged() {
  if (!prefs_) {
    return;
  }

  const bool show_search =
      prefs_->GetBoolean(kCustomNewTabShowSearchPrefPath);
  const bool show_logo =
      prefs_->GetBoolean(kCustomNewTabShowLogoPrefPath);
  const bool show_shortcuts =
      prefs_->GetBoolean(kCustomNewTabShowShortcutsPrefPath);
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
    Browser* browser = window->GetBrowserForMigrationOnly();
    if (!browser || !browser->profile() ||
        browser->profile()->GetPrefs() != prefs_) {
      return true;
    }

    TabStripModel* tab_strip_model = browser->tab_strip_model();
    if (!tab_strip_model) {
      return true;
    }

    for (int index = 0; index < tab_strip_model->count(); ++index) {
      content::WebContents* web_contents =
          tab_strip_model->GetWebContentsAt(index);
      if (!web_contents) {
        continue;
      }

      const GURL& url = web_contents->GetVisibleURL();
      const std::string_view host = url.host();
      if (host != chrome::kChromeUINewTabPageHost &&
          host != chrome::kChromeUINewTabHost) {
        continue;
      }

      content::WebUI* web_ui = web_contents->GetWebUI();
      if (!web_ui || !web_ui->CanCallJavascript()) {
        continue;
      }

      web_ui->CallJavascriptFunctionUnsafe(
          "customBrowserUpdateNtpPrefs", show_search, show_logo,
          show_shortcuts);
    }
    return true;
  });
}

NativeViewParamsPtr* CustomBrowserUiPrefs::FindExistingLayout(
    const NativeViewParamsPtr& layout) {
  if (!layout) {
    return nullptr;
  }
  for (auto& existing : ui_layout_params_ptr_->child_layouts) {
    if (SameNativeView(*existing, *layout)) {
      return &existing;
    }
  }
  return nullptr;
}

void CustomBrowserUiPrefs::UpdateLayouts(
    std::vector<NativeViewParamsPtr> batch) {
  // Report read-only pointers to observers after applying all changes.
  std::vector<NativeViewParams*> layouts_changed;
  for (size_t i = 0; i < batch.size(); ++i) {
    if (!batch[i]) {
      continue;
    }
    touched_hash_ids_.insert(batch[i].get()->hash_id);
    for (size_t j = i + 1; j < batch.size(); ++j) {
      if (batch[j] && SameNativeView(*batch[i], *batch[j])) {
        LOG(WARNING)
            << "Duplicate NativeViewParams in UpdateLayouts for view_id="
            << (batch[i]->view_id.has_value() ? *batch[i]->view_id : "null")
            << ", view_class_name="
            << (batch[i]->view_class_name.has_value()
                    ? *batch[i]->view_class_name
                    : "null")
            << ". Ignoring duplicate.";
        continue;
      }
    }

    // LOG(ERROR) << " UpdateLayouts: view_id=" <<
    // (batch[i]->view_id.has_value() ? *batch[i]->view_id : "null")
    //            << ", view_class_name=" <<
    //            (batch[i]->view_class_name.has_value() ?
    //            *batch[i]->view_class_name : "null")
    //            << ", visible=" << batch[i]->visible;

    NativeViewParamsPtr* existing = FindExistingLayout(batch[i]);
    if (!existing) {
      // New entry: clone and keep a pointer to the pointee we just added.
      AddLayout(batch[i]);
    } else if (!mojo::Equals(*existing, batch[i])) {
      // Reassign the StructPtr. This invalidates old_ptr.
      UpdateLayout(existing, batch[i]);
    } else {
      DCHECK((*existing)->visible == batch[i]->visible);
      DCHECK((*existing)->view_class_name == batch[i]->view_class_name);
      DCHECK((*existing)->view_id == batch[i]->view_id);
      DCHECK((*existing)->params == batch[i]->params);
      DCHECK((*existing)->hash_id == batch[i]->hash_id);
    }
  }
}

bool CustomBrowserUiPrefs::IsValidLayout(const NativeViewParams* layout) const {
  if (!layout) {
    return false;
  }
  if (!layout->view_class_name && !layout->view_id) {
    LOG(ERROR) << "Invalid layout: missing both view_class_name and view_id";
    return false;
  }

  for (auto& item : ui_layout_params_ptr_->child_layouts) {
    if (item.get() == layout) {
      return true;
    }
  }
  return false;
}

void CustomBrowserUiPrefs::AddObserver(Observer* obs) {
  observers_.AddObserver(obs);
}

void CustomBrowserUiPrefs::RemoveObserver(Observer* obs) {
  observers_.RemoveObserver(obs);
}
