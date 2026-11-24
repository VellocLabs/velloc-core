// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_NEXUS_GUEST_GROUPS_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_NEXUS_GUEST_GROUPS_H_

#include <memory>
#include <string_view>

#include "base/memory/scoped_refptr.h"
#include "base/supports_user_data.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "custom_browser/chromium_hooks/hook_dispatcher.h"

namespace custom_browser {

// Contract: the two process groups of the "nexus" guest partition (P-2).
//   kRuntime  the runtime host guest and every SharedWorker of the partition
//   kUi       guests named "nexus-ui"
// Each group has one guest SiteInstance, in a BrowsingInstance of its own, so
// the two never share a renderer process whichever is asked for first. The
// instances are held as BrowserContext user data from EnableNexusGuestGroups
// to DisableNexusGuestGroups; outside that window, and for any other
// partition, the lookup returns null and the caller keeps its stock
// behaviour. UI thread only.
enum class NexusGuestGroup { kRuntime, kUi };

inline constexpr std::string_view kNexusGuestPartitionName = "nexus";
inline constexpr std::string_view kNexusUiGuestName = "nexus-ui";

// Starts holding group SiteInstances for `context`. Idempotent.
inline void EnableNexusGuestGroups(content::BrowserContext* context) {
  const void* key = NexusGuestGroupsUserDataKey(NexusGuestGroupsSlot::kEnabled);
  if (!context->GetUserData(key)) {
    context->SetUserData(key, std::make_unique<base::SupportsUserData::Data>());
  }
}

// Drops every held SiteInstance. Must run before `context` is destroyed.
inline void DisableNexusGuestGroups(content::BrowserContext* context) {
  for (NexusGuestGroupsSlot slot :
       {NexusGuestGroupsSlot::kEnabled, NexusGuestGroupsSlot::kRuntime,
        NexusGuestGroupsSlot::kUi}) {
    context->RemoveUserData(NexusGuestGroupsUserDataKey(slot));
  }
}

// The group's SiteInstance, created on first use. Null when the groups are
// not enabled, `config` is not a "nexus" partition, or the group already
// belongs to another "nexus" partition of this context.
inline scoped_refptr<content::SiteInstance> GetNexusGuestGroupSiteInstance(
    content::BrowserContext* context,
    const content::StoragePartitionConfig& config,
    NexusGuestGroup group) {
  if (config.partition_name() != kNexusGuestPartitionName ||
      !context->GetUserData(
          NexusGuestGroupsUserDataKey(NexusGuestGroupsSlot::kEnabled))) {
    return nullptr;
  }
  const void* key = NexusGuestGroupsUserDataKey(
      group == NexusGuestGroup::kUi ? NexusGuestGroupsSlot::kUi
                                    : NexusGuestGroupsSlot::kRuntime);
  using Held = base::UserDataAdapter<content::SiteInstance>;
  if (content::SiteInstance* held = Held::Get(context, key)) {
    return held->GetSecurityPrincipal().GetStoragePartitionConfig() == config
               ? held
               : nullptr;
  }
  scoped_refptr<content::SiteInstance> created =
      content::SiteInstance::CreateForGuest(context, config);
  context->SetUserData(key, std::make_unique<Held>(created));
  return created;
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_NEXUS_GUEST_GROUPS_H_
