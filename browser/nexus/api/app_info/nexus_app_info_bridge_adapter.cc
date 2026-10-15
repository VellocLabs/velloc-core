#include "custom_browser/browser/nexus/api/app_info/nexus_app_info_bridge_adapter.h"

#include <string>
#include <utility>
#include <vector>

#include "base/memory/memory_pressure_listener.h"
#include "base/strings/strcat.h"
#include "base/system/sys_info.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/version_info.h"
#include "custom_browser/browser/nexus/api/app_info/nexus_render_cache_purger.h"
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

  // F-24: who this machine is, for a remote page's topbar tooltip. Each value is
  // omitted, not sent empty, when the platform has no answer. `net::GetHostName`
  // is a local syscall with no name-service lookup, so it is safe on UI.
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

namespace {

struct PurgeTargetName {
  NexusRenderCachePurger::Target target;
  const char* name;
};

constexpr PurgeTargetName kPurgeTargetNames[] = {
    {NexusRenderCachePurger::kGuest, "guest"},
    {NexusRenderCachePurger::kGpu, "gpu"},
    {NexusRenderCachePurger::kAgentTabs, "agentTabs"},
};

base::ListValue PurgeTargetList(uint32_t targets) {
  base::ListValue list;
  for (const PurgeTargetName& entry : kPurgeTargetNames) {
    if (targets & entry.target) {
      list.Append(entry.name);
    }
  }
  return list;
}

}  // namespace

void NexusAppInfoBridgeAdapter::HandlePurgeRenderCachesOnUI(
    ::nexus::generated::PurgeRenderCachesInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  uint32_t targets = 0;
  if (!input.targets.has_value()) {
    targets = NexusRenderCachePurger::kGuest | NexusRenderCachePurger::kGpu;
  }
  for (const std::string& name : input.targets.value_or(
           std::vector<std::string>())) {
    bool known = false;
    for (const PurgeTargetName& entry : kPurgeTargetNames) {
      if (name == entry.name) {
        targets |= entry.target;
        known = true;
      }
    }
    if (!known) {
      std::move(done).InvalidArgs(
          base::StrCat({"appInfo.purgeRenderCaches: unknown target '", name,
                        "'"}));
      return;
    }
  }

  base::MemoryPressureLevel level =
      base::MEMORY_PRESSURE_LEVEL_CRITICAL;
  if (input.level.has_value() && *input.level != "critical") {
    if (*input.level != "moderate") {
      std::move(done).InvalidArgs(
          base::StrCat({"appInfo.purgeRenderCaches: unknown level '",
                        *input.level, "'"}));
      return;
    }
    level = base::MEMORY_PRESSURE_LEVEL_MODERATE;
  }

  const NexusRenderCachePurger::Result result =
      NexusRenderCachePurger::Purge(profile_, targets, level);
  base::DictValue dict;
  dict.Set("purged", PurgeTargetList(result.purged));
  dict.Set("throttled", PurgeTargetList(result.throttled));
  dict.Set("guestProcesses", result.guest_processes);
  dict.Set("agentTabProcesses", result.agent_tab_processes);
  dict.Set("gpuProcesses", result.gpu_processes);
  std::move(done).Success(std::move(dict));
}

}  // namespace nexus
