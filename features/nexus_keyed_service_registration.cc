// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/features/nexus_keyed_service_registration.h"

#include "base/check.h"
#include "chrome/browser/browser_process.h"
#include "custom_browser/browser/nexus/api/alarms/nexus_alarm_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tracker.h"

namespace nexus {

void RegisterNexusKeyedServiceFactories() {
  // Cheap proxy for "BrowserProcessImpl::Init() has run": g_browser_process is
  // null throughout ChromeBrowserMainParts::Create(), which is where the
  // previous, eager registration lived and where ThemeServiceFactory ->
  // ChromeExtensionSystemFactory DCHECK-crashed the browser at startup.
  CHECK(g_browser_process);

  LlmBrokerFactory::GetInstance();
  NexusSessionTabTrackerFactory::GetInstance();
  NexusBackgroundPanelHostFactory::GetInstance();
  // Created with the profile: alarms missed while the browser was off fire
  // at startup (api/alarms/nexus_alarm_service.h).
  NexusAlarmServiceFactory::GetInstance();
}

NexusKeyedServiceLifecycleHook::NexusKeyedServiceLifecycleHook() = default;
NexusKeyedServiceLifecycleHook::~NexusKeyedServiceLifecycleHook() = default;

void NexusKeyedServiceLifecycleHook::PreCreateThreads() {
  RegisterNexusKeyedServiceFactories();
}

}  // namespace nexus
