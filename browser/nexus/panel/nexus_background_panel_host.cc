// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"

#include <utility>

#include "base/check.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/common/pref_names.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"

namespace nexus {

NexusBackgroundPanelHost::NexusBackgroundPanelHost(Profile* profile)
    : profile_(profile) {
  CHECK(profile_);
}

NexusBackgroundPanelHost::~NexusBackgroundPanelHost() {
  // Shutdown() normally ran already; be safe for tests that delete directly.
  DropParked();
}

// static
bool NexusBackgroundPanelHost::IsBackgroundPanelEnabled() {
  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  return local_state && local_state->GetBoolean(prefs::kBackgroundModeEnabled);
}

void NexusBackgroundPanelHost::Park(
    std::unique_ptr<content::WebContents> contents) {
  CHECK(contents);
  DropParked();
  parked_ = std::move(contents);
  // Keep the profile resident while the page lives without a window; the
  // keep-alive is released with the contents (Take / DropParked).
  profile_keep_alive_ = std::make_unique<ScopedProfileKeepAlive>(
      profile_, ProfileKeepAliveOrigin::kNexusBackgroundPanel);
  content::WebContentsObserver::Observe(parked_.get());
}

std::unique_ptr<content::WebContents> NexusBackgroundPanelHost::Take() {
  content::WebContentsObserver::Observe(nullptr);
  std::unique_ptr<content::WebContents> contents = std::move(parked_);
  profile_keep_alive_.reset();
  return contents;
}

void NexusBackgroundPanelHost::Shutdown() {
  DropParked();
}

void NexusBackgroundPanelHost::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  DropParked();
}

void NexusBackgroundPanelHost::DropParked() {
  content::WebContentsObserver::Observe(nullptr);
  // Contents first (it may still reach into the profile), then the
  // keep-alive that guaranteed the profile was there for it.
  parked_.reset();
  profile_keep_alive_.reset();
}

// static
NexusBackgroundPanelHost* NexusBackgroundPanelHostFactory::GetForProfile(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<NexusBackgroundPanelHost*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
NexusBackgroundPanelHostFactory*
NexusBackgroundPanelHostFactory::GetInstance() {
  static base::NoDestructor<NexusBackgroundPanelHostFactory> instance;
  return instance.get();
}

NexusBackgroundPanelHostFactory::NexusBackgroundPanelHostFactory()
    : BrowserContextKeyedServiceFactory(
          "NexusBackgroundPanelHost",
          BrowserContextDependencyManager::GetInstance()) {
  // A parked page is destroyed in Shutdown(); its page handlers unobserve
  // the ThemeService then, so it must still be up (glic declares the same).
  DependsOn(ThemeServiceFactory::GetInstance());
}

NexusBackgroundPanelHostFactory::~NexusBackgroundPanelHostFactory() = default;

std::unique_ptr<KeyedService>
NexusBackgroundPanelHostFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  // The kNexus window is a regular-profile surface; never park incognito.
  if (!profile || profile->IsOffTheRecord()) {
    return nullptr;
  }
  return std::make_unique<NexusBackgroundPanelHost>(profile);
}

}  // namespace nexus
