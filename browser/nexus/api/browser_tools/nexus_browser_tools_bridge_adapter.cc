#include "custom_browser/browser/nexus/api/browser_tools/nexus_browser_tools_bridge_adapter.h"

#include <utility>

#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"

namespace nexus {

namespace {

// Build the effective-values dict. Today: just `separateWindow`.
// When a future toggle lands, extend this builder (don't fork the
// snapshot vs setField shapes).
base::DictValue BuildSnapshotValues(PrefService* prefs) {
  base::DictValue values;
  const bool separate_window =
      prefs && prefs->GetBoolean(prefs::kNexusBrowserToolsSeparateWindow);
  values.Set("separateWindow", separate_window);
  return values;
}

}  // namespace

NexusBrowserToolsBridgeAdapter::NexusBrowserToolsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::BrowserToolsApiBase(delegate), profile_(profile) {}

NexusBrowserToolsBridgeAdapter::~NexusBrowserToolsBridgeAdapter() = default;

void NexusBrowserToolsBridgeAdapter::HandleSnapshotOnUI(
    ::nexus::generated::SnapshotInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("BROWSER_TOOLS_ERROR", "Profile unavailable");
    return;
  }
  base::DictValue result;
  result.Set("values", BuildSnapshotValues(profile_->GetPrefs()));
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusBrowserToolsBridgeAdapter::HandleSetFieldOnUI(
    ::nexus::generated::SetFieldInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("BROWSER_TOOLS_ERROR", "Profile unavailable");
    return;
  }
  PrefService* prefs = profile_->GetPrefs();
  if (!prefs) {
    std::move(done).Error("BROWSER_TOOLS_ERROR", "Profile prefs unavailable");
    return;
  }

  // `separateWindow` — bool|null. null clears the override (returns to
  // the runtime default, which is `false`). The pref is a non-nullable
  // boolean, so null maps to ClearPref() which restores the registered
  // default.
  if (input.patch.contains("separateWindow")) {
    const base::Value* v = input.patch.Find("separateWindow");
    if (v && v->is_bool()) {
      prefs->SetBoolean(prefs::kNexusBrowserToolsSeparateWindow,
                        v->GetBool());
    } else if (v && v->is_none()) {
      prefs->ClearPref(prefs::kNexusBrowserToolsSeparateWindow);
    } else {
      std::move(done).Error(
          "BROWSER_TOOLS_ERROR",
          "separateWindow must be a boolean or null");
      return;
    }
  }

  base::DictValue result;
  result.Set("ok", true);
  result.Set("values", BuildSnapshotValues(prefs));
  std::move(done).Success(base::Value(std::move(result)));
}

}  // namespace nexus
