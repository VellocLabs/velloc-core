#include "custom_browser/browser/nexus/api/system_prefs/nexus_system_prefs_bridge_adapter.h"

#include <utility>

#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"

namespace nexus {

NexusSystemPrefsBridgeAdapter::NexusSystemPrefsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* /*profile*/)
    : ::nexus::generated::SystemPrefsApiBase(delegate) {}

NexusSystemPrefsBridgeAdapter::~NexusSystemPrefsBridgeAdapter() = default;

void NexusSystemPrefsBridgeAdapter::HandleGetOnUI(
    ::nexus::generated::GetInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PrefService* local_state = g_browser_process->local_state();
  bool background_mode = false;
#if BUILDFLAG(ENABLE_BACKGROUND_MODE)
  // Default is true; mirrors NexusSettingsHandler::HandleGetBackgroundModeEnabled.
  background_mode =
      local_state && local_state->GetBoolean(::prefs::kBackgroundModeEnabled);
#endif
  // `launcherEnabled` is a wire-compatibility alias of the same pref since the
  // two System toggles were merged; the legacy
  // nexus.launcher_enabled pref is migrated once by NexusBackgroundModeManager
  // and no longer read.
  base::DictValue result;
  result.Set("backgroundModeEnabled", background_mode);
  result.Set("launcherEnabled", background_mode);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSystemPrefsBridgeAdapter::HandleSetBackgroundModeOnUI(
    ::nexus::generated::SetBackgroundModeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
#if BUILDFLAG(ENABLE_BACKGROUND_MODE)
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    std::move(done).Error("SYSTEM_PREFS_ERROR", "local_state unavailable");
    return;
  }
  // BackgroundModeEnabled is an enterprise policy: a managed value cannot be
  // changed here, and a write under it would leave the UI showing a state
  // the host never adopts. Refuse, so the toggle rolls back.
  if (!local_state->IsUserModifiablePreference(
          ::prefs::kBackgroundModeEnabled)) {
    std::move(done).Error("SYSTEM_PREFS_MANAGED",
                          "background mode is set by policy");
    return;
  }
  // The BackgroundModeManager's PrefChangeRegistrar on local_state reacts.
  local_state->SetBoolean(::prefs::kBackgroundModeEnabled, input.enabled);
#endif
  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSystemPrefsBridgeAdapter::HandleSetLauncherOnUI(
    ::nexus::generated::SetLauncherInput input,
    std::vector<blink::MessagePortDescriptor> ports,
    ::nexus::BridgeResponseCallback done) {
  // Alias of setBackgroundMode (the toggles were merged): one pref drives the
  // keep-alive + tray icon (NexusBackgroundModeManager) and the background
  // panel (NexusBackgroundPanelHost).
  ::nexus::generated::SetBackgroundModeInput forwarded;
  forwarded.enabled = input.enabled;
  HandleSetBackgroundModeOnUI(std::move(forwarded), std::move(ports),
                              std::move(done));
}

}  // namespace nexus
