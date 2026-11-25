// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"

#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "base/callback_list.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_multi_source_observation.h"
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
#include "content/public/browser/dedicated_worker_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
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

base::RepeatingClosureList& StateChangedCallbacks() {
  static base::NoDestructor<base::RepeatingClosureList> callbacks;
  return *callbacks;
}

// Every live host, so a lookup by contents never goes through the keyed
// service factory (which must not be asked while a profile shuts down).
std::vector<NexusBackgroundPanelHost*>& LiveHosts() {
  static base::NoDestructor<std::vector<NexusBackgroundPanelHost*>> hosts;
  return *hosts;
}

}  // namespace

// The dedicated workers that frames of a parked page's guests created: the
// run_js and mod workers spawned through that window's spawner frames, and
// the window's own import worker. Each one is work a session still runs
// inside the parked page.
class NexusBackgroundPanelHost::ParkedWorkerTracker
    : public content::DedicatedWorkerService::Observer {
 public:
  explicit ParkedWorkerTracker(content::WebContents* parked) : parked_(parked) {
    for (content::WebContents* guest : parked_->GetInnerWebContents()) {
      content::DedicatedWorkerService* service =
          guest->GetPrimaryMainFrame()
              ->GetStoragePartition()
              ->GetDedicatedWorkerService();
      if (observations_.IsObservingSource(service)) {
        continue;
      }
      observations_.AddObservation(service);
      service->EnumerateDedicatedWorkers(this);
    }
  }

  bool has_workers() const { return !workers_.empty(); }

  // content::DedicatedWorkerService::Observer:
  void OnWorkerCreated(const blink::DedicatedWorkerToken& worker_token,
                       content::ChildProcessId worker_process_id,
                       const url::Origin& security_origin,
                       content::DedicatedWorkerCreator creator) override {
    if (!IsCreatedInParkedPage(creator)) {
      return;
    }
    workers_.insert(worker_token);
    if (workers_.size() == 1) {
      NexusBackgroundPanelHost::NotifyStateChanged();
    }
  }
  void OnBeforeWorkerDestroyed(const blink::DedicatedWorkerToken& worker_token,
                               content::DedicatedWorkerCreator creator) override {
    if (workers_.erase(worker_token) && workers_.empty()) {
      NexusBackgroundPanelHost::NotifyStateChanged();
    }
  }
  void OnFinalResponseURLDetermined(
      const blink::DedicatedWorkerToken& worker_token,
      const GURL& url) override {}

 private:
  bool IsCreatedInParkedPage(const content::DedicatedWorkerCreator& creator) {
    if (const auto* token = std::get_if<blink::DedicatedWorkerToken>(&creator)) {
      return workers_.contains(*token);
    }
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(
        std::get<content::GlobalRenderFrameHostId>(creator));
    content::WebContents* contents =
        frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
    return contents && contents->GetOuterWebContents() == parked_;
  }

  const raw_ptr<content::WebContents> parked_;
  base::ScopedMultiSourceObservation<content::DedicatedWorkerService,
                                     content::DedicatedWorkerService::Observer>
      observations_{this};
  std::set<blink::DedicatedWorkerToken> workers_;
};

// Reloads the runtime host page when its own renderer goes away. The guest
// inside reloads itself; this covers the page around it.
class NexusBackgroundPanelHost::RuntimeHostObserver
    : public content::WebContentsObserver {
 public:
  explicit RuntimeHostObserver(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}

  void set_reload_enabled(bool enabled) { reload_enabled_ = enabled; }

  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override {
    NexusBackgroundPanelHost::NotifyStateChanged();
    if (!reload_enabled_) {
      return;
    }
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

  void PrimaryPageChanged(content::Page& page) override {
    NexusBackgroundPanelHost::NotifyStateChanged();
  }

 private:
  void Reload() {
    if (web_contents()) {
      web_contents()->GetController().Reload(content::ReloadType::NORMAL,
                                             /*check_for_repost=*/false);
    }
  }

  bool reload_enabled_ = true;
  base::TimeTicks window_start_;
  int reloads_ = 0;
  base::WeakPtrFactory<RuntimeHostObserver> weak_factory_{this};
};

NexusBackgroundPanelHost::NexusBackgroundPanelHost(Profile* profile)
    : profile_(profile) {
  CHECK(profile_);
  custom_browser::EnableNexusGuestGroups(profile_);
  LiveHosts().push_back(this);
}

NexusBackgroundPanelHost::~NexusBackgroundPanelHost() {
  std::erase(LiveHosts(), this);
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
  if (has_runtime_host()) {
    return;
  }
  // A page whose renderer is gone and that its observer gave up on is
  // replaced, so opening a window always brings the runtime back. Its
  // keep-alive carries over to the new page.
  runtime_host_observer_.reset();
  runtime_host_.reset();
  if (!runtime_keep_alive_) {
    runtime_keep_alive_ = std::make_unique<ScopedProfileKeepAlive>(
        profile_, ProfileKeepAliveOrigin::kNexusBackgroundPanel);
  }
  content::WebContents::CreateParams params(profile_);
  params.initially_hidden = true;
  runtime_host_ = content::WebContents::Create(params);
  runtime_host_observer_ =
      std::make_unique<RuntimeHostObserver>(runtime_host_.get());
  runtime_host_observer_->set_reload_enabled(runtime_host_reload_enabled_);
  runtime_host_->GetController().LoadURL(
      runtime_host_url_for_testing_.is_valid() ? runtime_host_url_for_testing_
                                               : RuntimeHostUrl(),
      content::Referrer(), ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  NotifyStateChanged();
}

bool NexusBackgroundPanelHost::has_runtime_host() const {
  return runtime_host_ && !runtime_host_->IsCrashed();
}

void NexusBackgroundPanelHost::set_runtime_host_reload_enabled_for_testing(
    bool enabled) {
  runtime_host_reload_enabled_ = enabled;
  if (runtime_host_observer_) {
    runtime_host_observer_->set_reload_enabled(enabled);
  }
}

void NexusBackgroundPanelHost::StopRuntimeHost() {
  const bool had_runtime_host = runtime_host_ != nullptr;
  runtime_host_observer_.reset();
  // Contents first (it may still reach into the profile), then the
  // keep-alive that guaranteed the profile was there for it.
  runtime_host_.reset();
  runtime_keep_alive_.reset();
  if (had_runtime_host) {
    NotifyStateChanged();
  }
}

// static
bool NexusBackgroundPanelHost::IsParkedAndIdle(
    content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  for (NexusBackgroundPanelHost* host : LiveHosts()) {
    if (host->parked_.get() == contents) {
      return host->has_runtime_host() &&
             !(host->parked_workers_ && host->parked_workers_->has_workers());
    }
  }
  return false;
}

// static
base::CallbackListSubscription
NexusBackgroundPanelHost::AddStateChangedCallback(
    base::RepeatingClosure callback) {
  return StateChangedCallbacks().Add(std::move(callback));
}

// static
void NexusBackgroundPanelHost::NotifyStateChanged() {
  StateChangedCallbacks().Notify();
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
  parked_workers_ = std::make_unique<ParkedWorkerTracker>(parked_.get());
  NotifyStateChanged();
}

std::unique_ptr<content::WebContents> NexusBackgroundPanelHost::Take() {
  content::WebContentsObserver::Observe(nullptr);
  parked_workers_.reset();
  std::unique_ptr<content::WebContents> contents = std::move(parked_);
  profile_keep_alive_.reset();
  if (contents) {
    NotifyStateChanged();
  }
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
  parked_workers_.reset();
  const bool had_parked = parked_ != nullptr;
  // Contents first (it may still reach into the profile), then the
  // keep-alive that guaranteed the profile was there for it.
  parked_.reset();
  profile_keep_alive_.reset();
  if (had_parked) {
    NotifyStateChanged();
  }
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
