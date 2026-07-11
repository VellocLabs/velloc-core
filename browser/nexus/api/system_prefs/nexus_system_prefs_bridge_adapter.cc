#include "custom_browser/browser/nexus/api/system_prefs/nexus_system_prefs_bridge_adapter.h"

#include <utility>

#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"

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
  const bool launcher =
      local_state &&
      local_state->GetBoolean(nexus::prefs::kNexusLauncherEnabled);

  base::DictValue result;
  result.Set("backgroundModeEnabled", background_mode);
  result.Set("launcherEnabled", launcher);
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
  // The BackgroundModeManager's PrefChangeRegistrar on local_state reacts.
  local_state->SetBoolean(::prefs::kBackgroundModeEnabled, input.enabled);
#endif
  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusSystemPrefsBridgeAdapter::HandleSetLauncherOnUI(
    ::nexus::generated::SetLauncherInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    std::move(done).Error("SYSTEM_PREFS_ERROR", "local_state unavailable");
    return;
  }
  // The NexusBackgroundModeManager's PrefChangeRegistrar on local_state reacts
  // (acquires/releases the keep-alive + tray icon).
  local_state->SetBoolean(nexus::prefs::kNexusLauncherEnabled, input.enabled);
  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

}  // namespace nexus
