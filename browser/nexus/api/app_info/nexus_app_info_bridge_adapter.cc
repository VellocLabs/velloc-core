#include "custom_browser/browser/nexus/api/app_info/nexus_app_info_bridge_adapter.h"

#include <string>
#include <utility>

#include "base/system/sys_info.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/version_info.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_client_restart.h"
#include "custom_browser/browser/nexus/nexus_webui_config.h"
#include "custom_browser/common/product_version.h"
#include "net/base/network_interfaces.h"

namespace nexus {

NexusAppInfoBridgeAdapter::NexusAppInfoBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::AppInfoApiBase(delegate), profile_(profile) {}

NexusAppInfoBridgeAdapter::~NexusAppInfoBridgeAdapter() = default;

void NexusAppInfoBridgeAdapter::HandleGetVersionInfoOnUI(
    ::nexus::generated::GetVersionInfoInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // `appVersion` is the fork's OWN product version (custom_browser/VERSION —
  // the same `customBrowserProductVersion` the chrome://settings About patch
  // shows); `chromiumVersion` is the Chromium base it's built on. They used
  // to both report the Chromium number, which made the guest About page show
  // the Chromium version as the client version.
  base::DictValue result;
  result.Set("appVersion",
             std::string(custom_browser::kCustomBrowserProductVersion));
  result.Set("chromiumVersion", std::string(version_info::GetVersionNumber()));

  // F-24: who this machine is, for a REMOTE page's topbar tooltip. The
  // runtime reads them once when the bridge connects and puts them in
  // `welcome.bootstrap` — nothing on the About page shows them, and the
  // guest cannot learn either on its own (a SharedWorker has no host name,
  // and `navigator` in it describes the browser, not the machine).
  //
  // Both are omitted rather than sent empty when the platform has no answer:
  // the tooltip drops a line it has no value for, and an empty string would
  // render as a blank row.
  //
  // `net::GetHostName` is `gethostname(2)` — a local syscall against the
  // configured name, with no name-service lookup — so it is safe on the UI
  // thread. `base::SysInfo`'s two reads are cached after first use.
  const std::string host_name = net::GetHostName();
  if (!host_name.empty()) {
    result.Set("hostName", host_name);
  }
  const std::string os_name = base::SysInfo::OperatingSystemName();
  const std::string os_version = base::SysInfo::OperatingSystemVersion();
  if (!os_name.empty() && !os_version.empty()) {
    result.Set("osName", os_name + " " + os_version);
  } else if (!os_name.empty() || !os_version.empty()) {
    result.Set("osName", os_name.empty() ? os_version : os_name);
  }

  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppInfoBridgeAdapter::HandleGetWebappUrlOnUI(
    ::nexus::generated::GetWebappUrlInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  const std::string url =
      prefs ? prefs->GetString(nexus::prefs::kNexusWebappUrl) : std::string();
  base::DictValue result;
  result.Set("url", url);
  // The built-in default guest URL — shown as the Agent URL field's hint /
  // placeholder when no override is set (mirrors the Lit page's
  // `customBrowserNexusWebappDefaultUrl`).
  result.Set("defaultUrl", nexus::kDefaultNexusGuestURL);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppInfoBridgeAdapter::HandleSetWebappUrlOnUI(
    ::nexus::generated::SetWebappUrlInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!prefs) {
    std::move(done).Error("APP_INFO_ERROR", "Profile prefs unavailable");
    return;
  }
  prefs->SetString(nexus::prefs::kNexusWebappUrl, input.url);
  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusAppInfoBridgeAdapter::HandleRelaunchOnUI(
    ::nexus::generated::RelaunchInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Reply BEFORE restarting, and rely on `RestartClient` posting rather than
  // running inline. Every BridgeResponseCallback terminal PostTasks its
  // dispatch to UI, so firing `done` here only *queues* the response; the
  // restart queued after it on the same runner therefore lands second, and the
  // guest's promise is answered instead of being dropped by teardown.
  std::move(done).SuccessVoid();
  RestartClient();
}

}  // namespace nexus
