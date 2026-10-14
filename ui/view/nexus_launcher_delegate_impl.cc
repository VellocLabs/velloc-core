// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/ui/view/nexus_launcher_delegate_impl.h"

#include <utility>

#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "custom_browser/ui/view/nexus_quit_confirmation.h"

namespace custom_browser {

namespace {

// The profile to launch windows for. Prefers an already-loaded profile (the
// launcher keep-alive keeps the last-used profile resident); falls back to the
// last-used profile otherwise. May be null (window_util tolerates it).
Profile* LaunchProfile() {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager) {
    return nullptr;
  }
  Profile* profile = profile_manager->GetLastUsedProfileIfLoaded();
  return profile ? profile : profile_manager->GetLastUsedProfile();
}

class NexusLauncherDelegateImpl : public nexus::NexusLauncherDelegate {
 public:
  void OpenOrFocusNexusWindow() override {
    window_util::OpenOrFocusNexusWindow(LaunchProfile());
  }

  void OpenOrFocusNexusWindowForProfile(Profile* profile) override {
    window_util::OpenOrFocusNexusWindow(profile ? profile : LaunchProfile());
  }

  void OpenOrFocusNormalWindow() override {
    window_util::OpenOrFocusNormalWindow(LaunchProfile());
  }

  void ConfirmQuit(base::OnceClosure proceed) override {
    ConfirmQuitWithRemoteDevices(std::move(proceed));
  }

  void Quit() override {
    // The manager has already released the launcher keep-alive; closing the
    // windows lets the process exit (other keep-alives still gate it).
    chrome::CloseAllBrowsers();
  }
};

}  // namespace

void InstallNexusLauncherDelegate() {
  static base::NoDestructor<NexusLauncherDelegateImpl> instance;
  nexus::SetNexusLauncherDelegate(instance.get());
}

}  // namespace custom_browser
