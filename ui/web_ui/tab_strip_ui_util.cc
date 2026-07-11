// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/ui/web_ui/tab_strip_ui_util.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_group_sync/tab_group_sync_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/saved_tab_groups/public/tab_group_sync_service.h"
#include "components/tab_groups/tab_group_id.h"
#include "ui/gfx/range/range.h"

namespace tab_strip_ui {

std::optional<tab_groups::TabGroupId> GetTabGroupIdFromString(
    TabGroupModel* tab_group_model,
    const std::string& group_id_string) {
  if (!tab_group_model) {
    return std::nullopt;
  }
  for (tab_groups::TabGroupId candidate : tab_group_model->ListTabGroups()) {
    if (candidate.ToString() == group_id_string) {
      return std::optional<tab_groups::TabGroupId>{candidate};
    }
  }

  return std::nullopt;
}

BrowserWindowInterface* GetBrowserWithGroupId(
    Profile* profile,
    const std::string& group_id_string) {
  BrowserWindowInterface* browser_with_group = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (profile && browser->GetProfile() != profile) {
          return true;
        }

        std::optional<tab_groups::TabGroupId> group_id =
            GetTabGroupIdFromString(browser->GetTabStripModel()->group_model(),
                                    group_id_string);
        if (group_id.has_value()) {
          browser_with_group = browser;
        }
        return !browser_with_group;
      });

  return browser_with_group;
}

void MoveGroupAcrossWindows(BrowserWindowInterface* source_browser,
                            BrowserWindowInterface* target_browser,
                            int to_index,
                            const tab_groups::TabGroupId& group_id) {
  TabStripModel* const target_tab_strip = target_browser->GetTabStripModel();

  if (!target_tab_strip->SupportsTabGroups()) {
    return;
  }

  const std::optional<tab_groups::TabGroupId> next_tab_dst_group =
      target_tab_strip->GetTabGroupForTab(to_index);
  const std::optional<tab_groups::TabGroupId> prev_tab_dst_group =
      target_tab_strip->GetTabGroupForTab(to_index - 1);

  // Check whether group contiguity will be respected in the `target_browser`
  // before performing the operation. Noop if the operation will result in a
  // split tab group.
  if (next_tab_dst_group.has_value() && prev_tab_dst_group.has_value() &&
      next_tab_dst_group == prev_tab_dst_group) {
    return;
  }

  tab_groups::TabGroupSyncService* const tab_group_service =
      tab_groups::TabGroupSyncServiceFactory::GetForProfile(
          source_browser->GetProfile());

  std::unique_ptr<tab_groups::ScopedLocalObservationPauser> observation_pauser;
  if (tab_group_service && tab_group_service->GetGroup(group_id)) {
    observation_pauser = tab_group_service->CreateScopedLocalObserverPauser();
  }

  TabStripModel* const source_tab_strip = source_browser->GetTabStripModel();
  std::unique_ptr<DetachedTabCollection> detached_group =
      source_tab_strip->DetachTabGroupForInsertion(group_id);
  target_tab_strip->InsertDetachedTabGroupAt(std::move(detached_group),
                                             to_index);
}

void MoveTabAcrossWindows(BrowserWindowInterface* source_browser,
                          int from_index,
                          BrowserWindowInterface* target_browser,
                          int to_index,
                          std::optional<tab_groups::TabGroupId> to_group_id) {
  TabStripModel* const source_tab_strip = source_browser->GetTabStripModel();
  const bool was_active = source_tab_strip->active_index() == from_index;
  const bool was_pinned = source_tab_strip->IsTabPinned(from_index);

  TabStripModel* const target_tab_strip = target_browser->GetTabStripModel();

  if (target_tab_strip->SupportsTabGroups()) {
    std::optional<tab_groups::TabGroupId> next_tab_dst_group =
        target_tab_strip->GetTabGroupForTab(to_index);
    std::optional<tab_groups::TabGroupId> prev_tab_dst_group =
        target_tab_strip->GetTabGroupForTab(to_index - 1);

    // Check whether group contiguity will be respected in the `target_browser`
    // before performing the operation. Noop if the operation will result in a
    // split tab group.
    if (next_tab_dst_group.has_value() && prev_tab_dst_group.has_value() &&
        next_tab_dst_group == prev_tab_dst_group &&
        to_group_id != prev_tab_dst_group) {
      return;
    }
  }

  std::unique_ptr<tabs::TabModel> detached_tab =
      source_tab_strip->DetachTabAtForInsertion(from_index);

  int add_types = AddTabTypes::ADD_NONE;
  if (was_active) {
    add_types |= AddTabTypes::ADD_ACTIVE;
  }
  if (was_pinned) {
    add_types |= AddTabTypes::ADD_PINNED;
  }

  target_tab_strip->InsertDetachedTabAt(to_index, std::move(detached_tab),
                                        add_types, to_group_id);
}

}  // namespace tab_strip_ui
