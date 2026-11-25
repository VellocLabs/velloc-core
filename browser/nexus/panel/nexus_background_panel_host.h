// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_BACKGROUND_PANEL_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_BACKGROUND_PANEL_HOST_H_

#include <memory>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

class Profile;
class ScopedProfileKeepAlive;

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace nexus {

// Per-profile parking slot for the kNexus window's top-chrome WebContents. On
// close with prefs::kBackgroundModeEnabled set, the window hands its contents
// here instead of destroying them; the next kNexus window `Take()`s them back,
// so the guest and its sessions survive. Holds a ScopedProfileKeepAlive so the
// Profile is not unloaded under a parked page. Also owns the profile's runtime
// host page and turns on the partition's two guest process groups.
class NexusBackgroundPanelHost : public KeyedService,
                                 public content::WebContentsObserver {
 public:
  explicit NexusBackgroundPanelHost(Profile* profile);
  ~NexusBackgroundPanelHost() override;

  NexusBackgroundPanelHost(const NexusBackgroundPanelHost&) = delete;
  NexusBackgroundPanelHost& operator=(const NexusBackgroundPanelHost&) =
      delete;

  // The System toggle: local_state `prefs::kBackgroundModeEnabled`
  // (Chromium registers it true by default). False when local_state is not
  // available (unit tests without a TestingBrowserProcess).
  static bool IsBackgroundPanelEnabled();

  // Takes ownership of a kNexus top-chrome WebContents whose window is going
  // away. Any previously parked contents is destroyed first (at most one
  // kNexus window per profile exists, so this only happens if a caller races
  // two closes). The contents must already be detached from its
  // views::WebView and have no delegate.
  void Park(std::unique_ptr<content::WebContents> contents);

  // Hands the parked contents (if any) to the caller, which becomes its
  // owner. Null when nothing is parked.
  std::unique_ptr<content::WebContents> Take();

  bool has_parked() const { return parked_ != nullptr; }

  // The profile's runtime host page: a top-chrome page that no window ever
  // shows. Its one guest holds the runtime SharedWorker's process, so the
  // worker outlives every window page (P-2). StartRuntimeHost() is
  // idempotent and takes a profile keep-alive; StopRuntimeHost() destroys the
  // page, and with it the runtime, and releases the keep-alive.
  void StartRuntimeHost();
  void StopRuntimeHost();
  // True while the page exists and its renderer has not gone away.
  bool has_runtime_host() const;
  // Whether this profile's sessions can be running with no window to show
  // them: a parked page, or a live runtime host page.
  bool runs_without_window() const {
    return has_parked() || has_runtime_host();
  }
  // Whether `contents` is a parked page that no session still needs: its
  // profile's runtime host page is live (so the host page holds the worker's
  // bridge port and spawner) and no frame of the parked page's guests has a
  // dedicated worker running (a run_js or mod worker spawned through this
  // window, or its import worker). False for a null `contents` or a page that
  // is not parked. (P-2)
  static bool IsParkedAndIdle(content::WebContents* contents);
  // Runs `callback` after a page is parked, taken or dropped, after the parked
  // page's first worker starts or its last one ends, and after the runtime
  // host page starts, stops, loses its renderer or commits again, in any
  // profile. UI thread.
  static base::CallbackListSubscription AddStateChangedCallback(
      base::RepeatingClosure callback);

  content::WebContents* runtime_host_for_testing() const {
    return runtime_host_.get();
  }

  // The URL the runtime host page loads.
  static GURL RuntimeHostUrl();
  // Replaces that URL: a unit test has no top-chrome page to load.
  void set_runtime_host_url_for_testing(const GURL& url) {
    runtime_host_url_for_testing_ = url;
  }
  content::WebContents* parked_for_testing() const { return parked_.get(); }
  void set_runtime_host_reload_enabled_for_testing(bool enabled);

  // KeyedService:
  void Shutdown() override;

  // content::WebContentsObserver — a parked page whose renderer died is
  // worthless (nothing would recover it while hidden); drop it so the next
  // window loads fresh.
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;

 private:
  class RuntimeHostObserver;
  class ParkedWorkerTracker;

  void DropParked();
  static void NotifyStateChanged();

  const raw_ptr<Profile> profile_;
  std::unique_ptr<content::WebContents> parked_;
  std::unique_ptr<ParkedWorkerTracker> parked_workers_;
  std::unique_ptr<ScopedProfileKeepAlive> profile_keep_alive_;
  std::unique_ptr<content::WebContents> runtime_host_;
  std::unique_ptr<RuntimeHostObserver> runtime_host_observer_;
  std::unique_ptr<ScopedProfileKeepAlive> runtime_keep_alive_;
  GURL runtime_host_url_for_testing_;
  bool runtime_host_reload_enabled_ = true;
};

class NexusBackgroundPanelHostFactory
    : public BrowserContextKeyedServiceFactory {
 public:
  // Null for a null profile, an off-the-record profile, or once the profile is
  // shutting down (no service is created then).
  static NexusBackgroundPanelHost* GetForProfile(Profile* profile);
  static NexusBackgroundPanelHostFactory* GetInstance();

  NexusBackgroundPanelHostFactory(const NexusBackgroundPanelHostFactory&) =
      delete;
  NexusBackgroundPanelHostFactory& operator=(
      const NexusBackgroundPanelHostFactory&) = delete;

 private:
  friend class base::NoDestructor<NexusBackgroundPanelHostFactory>;

  NexusBackgroundPanelHostFactory();
  ~NexusBackgroundPanelHostFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_BACKGROUND_PANEL_HOST_H_
