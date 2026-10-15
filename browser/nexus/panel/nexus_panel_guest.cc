// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_panel_guest.h"

#include "components/guest_view/browser/guest_view_base.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"

namespace nexus {

namespace {

// Matches `<webview id="nexusPane" partition="persist:nexus">` in
// resources/browser_webui/pages/nexus/nexus_view.ts.
constexpr char kNexusGuestPartitionName[] = "nexus";

}  // namespace

bool IsNexusPanelGuest(const guest_view::GuestViewBase* guest,
                       content::RenderFrameHost& frame_in_guest) {
  if (!guest) {
    return false;
  }
  content::RenderFrameHost* owner = guest->owner_rfh();
  if (!owner || !owner->GetMainFrame()->GetLastCommittedURL().SchemeIs(
                    content::kChromeUIScheme)) {
    return false;
  }
  const content::StoragePartitionConfig partition_config =
      frame_in_guest.GetSiteInstance()
          ->GetSecurityPrincipal()
          .GetStoragePartitionConfig();
  return partition_config.partition_name() == kNexusGuestPartitionName;
}

bool IsNexusPanelGuestContents(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  return IsNexusPanelGuest(guest_view::GuestViewBase::FromWebContents(contents),
                           *contents->GetPrimaryMainFrame());
}

}  // namespace nexus
