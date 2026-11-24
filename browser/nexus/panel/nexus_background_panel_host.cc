// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/common/pref_names.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/chromium_hooks/nexus_guest_groups.h"
#include "custom_browser/common/url_constants.h"
#include "net/base/url_util.h"
#include "ui/base/page_transition_types.h"

namespace nexus {

namespace {

// A runtime host page whose renderer keeps dying is left alone after this
// many reloads inside the window.
constexpr int kRuntimeHostMaxReloads = 3;
constexpr base::TimeDelta kRuntimeHostReloadWindow = base::Minutes(1);

}  // namespace

// Reloads the runtime host page when its own renderer goes away. The guest
// inside reloads itself; this covers the page around it.
class NexusBackgroundPanelHost::RuntimeHostObserver
    : public content::WebContentsObserver {
 public:
  explicit RuntimeHostObserver(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}

  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override {
    const base::TimeTicks now = base::TimeTicks::Now();
    if (now - window_start_ > kRuntimeHostReloadWindow) {
      window_start_ = now;
      reloads_ = 0;
    }
    if (++reloads_ > kRuntimeHostMaxReloads) {
      return;
    }
    // Not from inside the notification: the frame tree is mid-teardown.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&RuntimeHostObserver::Reload,
                                  weak_factory_.GetWeakPtr()));
  }

 private:
  void Reload() {
    if (web_contents()) {
      web_contents()->GetController().Reload(content::ReloadType::NORMAL,
                                             /*check_for_repost=*/false);
    }
  }

  base::TimeTicks window_start_;
  int reloads_ = 0;
  base::WeakPtrFactory<RuntimeHostObserver> weak_factory_{this};
};

NexusBackgroundPanelHost::NexusBackgroundPanelHost(Profile* profile)
    : profile_(profile) {
  CHECK(profile_);
  custom_browser::EnableNexusGuestGroups(profile_);
}

NexusBackgroundPanelHost::~NexusBackgroundPanelHost() {
  // Shutdown() normally ran already; be safe for tests that delete directly.
  DropParked();
  StopRuntimeHost();
  custom_browser::DisableNexusGuestGroups(profile_);
}

// static
GURL NexusBackgroundPanelHost::RuntimeHostUrl() {
  return net::AppendQueryParameter(GURL(custom_browser::kChromeUICustomBrowserURL),
                                   "nexusRuntimeHost", "1");
}

void NexusBackgroundPanelHost::StartRuntimeHost() {
  if (runtime_host_) {
    return;
  }
  runtime_keep_alive_ = std::make_unique<ScopedProfileKeepAlive>(
      profile_, ProfileKeepAliveOrigin::kNexusBackgroundPanel);
  content::WebContents::CreateParams params(profile_);
  params.initially_hidden = true;
  runtime_host_ = content::WebContents::Create(params);
  runtime_host_observer_ =
      std::make_unique<RuntimeHostObserver>(runtime_host_.get());
  runtime_host_->GetController().LoadURL(
      runtime_host_url_for_testing_.is_valid() ? runtime_host_url_for_testing_
                                               : RuntimeHostUrl(),
      content::Referrer(), ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
}

void NexusBackgroundPanelHost::StopRuntimeHost() {
  runtime_host_observer_.reset();
  // Contents first (it may still reach into the profile), then the
  // keep-alive that guaranteed the profile was there for it.
  runtime_host_.reset();
  runtime_keep_alive_.reset();
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
  StopRuntimeHost();
  // The held SiteInstances go while the profile is still whole.
  custom_browser::DisableNexusGuestGroups(profile_);
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
