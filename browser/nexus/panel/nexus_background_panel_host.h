// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_BACKGROUND_PANEL_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_BACKGROUND_PANEL_HOST_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/web_contents_observer.h"

class Profile;
class ScopedProfileKeepAlive;

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace nexus {

// Per-profile parking slot for the kNexus window's top-chrome WebContents
// (the `chrome://custom.top-chrome/` page that embeds the nexus_web guest
// <webview>, whose SharedWorker runs the agent loop).
//
// Mirrors the slice of Chrome's glic that keeps the web client alive while
// the panel is closed: there the host WebContents is owned by the keyed
// service's `Host`, and closing the panel only does
// `WebView::SetWebContents(nullptr)`. Here the window's CustomBrowserWebUIView
// owns the contents while a window shows it, and hands it to this service —
// instead of destroying it — when the kNexus window closes with the System
// setting "Continue running background apps when main window is closed"
// (`prefs::kBackgroundModeEnabled`, local_state) enabled. The next kNexus
// window takes it back (`Take()`), so the guest, its SharedWorker and every
// running session survive the window; nothing reloads.
//
// Process lifetime is NOT this service's concern: the SAME pref also drives
// the launcher tray keep-alive (NexusBackgroundModeManager), so with the
// toggle on the process survives the last window and the tray icon is the
// way back in; a parked page is only ever destroyed on shutdown / profile
// teardown / renderer death, or when the toggle is turned off before the
// next window opens (ShouldParkForBackground is re-evaluated per close).
// What this service does hold is a ScopedProfileKeepAlive (glic's kGlicView
// pattern): a parked contents must not have its Profile unloaded from under
// it once no browser window references the profile any more.
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
  content::WebContents* parked_for_testing() const { return parked_.get(); }

  // KeyedService:
  void Shutdown() override;

  // content::WebContentsObserver — a parked page whose renderer died is
  // worthless (nothing would recover it while hidden); drop it so the next
  // window loads fresh.
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;

 private:
  void DropParked();

  const raw_ptr<Profile> profile_;
  std::unique_ptr<content::WebContents> parked_;
  std::unique_ptr<ScopedProfileKeepAlive> profile_keep_alive_;
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
