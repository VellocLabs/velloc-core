// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_PANEL_GUEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_PANEL_GUEST_H_

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace guest_view {
class GuestViewBase;
}  // namespace guest_view

namespace nexus {

// True when `guest` is the Nexus panel guest: the `persist:nexus` storage
// partition (`frame_in_guest` is any frame of the guest), owned by a
// chrome:// WebUI page (the browser shell).
//
// Security: regular tabs are native views in this fork, not guests, and an
// ordinary web page can never own a webview in a chrome://-owned partition,
// so partition + owner identify the panel. (P-2)
bool IsNexusPanelGuest(const guest_view::GuestViewBase* guest,
                       content::RenderFrameHost& frame_in_guest);

// Same test for a WebContents; false for anything that is not a guest.
bool IsNexusPanelGuestContents(content::WebContents* contents);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_PANEL_GUEST_H_
