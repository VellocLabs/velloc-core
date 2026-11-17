// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_APP_PAGE_FRAME_GUARD_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_APP_PAGE_FRAME_GUARD_H_

#include "base/containers/flat_set.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/web_contents_user_data.h"

class GURL;

namespace content {
class NavigationHandle;
class NavigationThrottleRegistry;
class RenderFrameHost;
}  // namespace content

namespace custom_browser {
class HookRegistrar;
}  // namespace custom_browser

namespace mojo {
template <typename ContextType>
class BinderMapWithContext;
}  // namespace mojo

namespace nexus {

// The `name` the guest gives the frame of a plugin app page (F-42).
inline constexpr char kAppPageFrameName[] = "velloc-app-page";

// The URLs an app-page frame may still load: about:srcdoc and about:blank,
// neither of which makes a request.
bool IsAllowedAppPageFrameUrl(const GURL& url);

// Security: the set of app-page frames in one WebContents (F-42). A child
// frame is latched the first time it is seen named `kAppPageFrameName` in the
// Nexus guest, which is when the guest adds it, before any of its documents
// runs a script. A latch lasts for the frame's lifetime: the page renaming its
// window never clears it. Every frame below a latched one is an app-page frame
// too. UI thread only.
class AppPageFrames : public content::WebContentsUserData<AppPageFrames> {
 public:
  ~AppPageFrames() override;

  // Latches `frame` when `in_nexus_guest` and it is a child frame named
  // `kAppPageFrameName`.
  static void Note(content::RenderFrameHost& frame, bool in_nexus_guest);

  // `frame` or one of its ancestors is latched.
  static bool Contains(content::RenderFrameHost& frame);

  // The frame `handle` navigates, or one of its ancestors, is latched.
  static bool ContainsNavigation(content::NavigationHandle& handle);

 private:
  friend class content::WebContentsUserData<AppPageFrames>;
  explicit AppPageFrames(content::WebContents* contents);

  bool Has(content::FrameTreeNodeId id) const { return latched_.contains(id); }

  base::flat_set<content::FrameTreeNodeId> latched_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

// Cancels a navigation in an app-page frame before its request starts, unless
// it goes to a URL `IsAllowedAppPageFrameUrl` accepts. Nothing is committed,
// so the page keeps running and its URL never leaves the machine.
class AppPageNavigationThrottle : public content::NavigationThrottle {
 public:
  explicit AppPageNavigationThrottle(
      content::NavigationThrottleRegistry& registry);
  ~AppPageNavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;
};

// Adds `AppPageNavigationThrottle` when the navigation is in an app-page frame.
void MaybeAddAppPageNavigationThrottle(
    content::NavigationThrottleRegistry& registry);

// When `frame` is an app-page frame, replaces its WebRTC socket manager with
// one that reports no network and opens no socket, and its DNS-prefetch /
// preconnect handler with one that ignores every hint.
void OverrideAppPageFrameBinders(
    content::RenderFrameHost& frame,
    mojo::BinderMapWithContext<content::RenderFrameHost*>& map);

// Installs the throttle provider and the binder override; the guest test is
// `IsNexusPanelGuestContents`.
void RegisterAppPageFrameGuardHooks(custom_browser::HookRegistrar* registrar);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_APP_PAGE_FRAME_GUARD_H_
