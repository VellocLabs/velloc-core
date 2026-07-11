// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// custom-browser: re-homed from chrome/browser/ui/webui/tab_strip/, which was
// removed in the Chromium 149 webui_tab_strip refactor. Only the four helpers
// the custom browser WebUI handler actually uses (cross-window tab/group moves
// and group-id lookup) are carried here.

#ifndef CUSTOM_BROWSER_UI_WEB_UI_TAB_STRIP_UI_UTIL_H_
#define CUSTOM_BROWSER_UI_WEB_UI_TAB_STRIP_UI_UTIL_H_

#include <optional>
#include <string>

#include "components/tab_groups/tab_group_id.h"

class BrowserWindowInterface;
class Profile;
class TabGroupModel;

namespace tab_strip_ui {

std::optional<tab_groups::TabGroupId> GetTabGroupIdFromString(
    TabGroupModel* tab_group_model,
    const std::string& group_id_string);

// Find the browser containing the group with ID |group_id_string| or nullptr if
// none. If the profile is not specified, find any browser containing the group.
BrowserWindowInterface* GetBrowserWithGroupId(
    Profile* profile,
    const std::string& group_id_string);

// Moves all the tabs in a group from `source_browser` to `target_browser` at
// `to_index`. If the operation results in a split tab group in the destination
// browser, noop instead.
void MoveGroupAcrossWindows(BrowserWindowInterface* source_browser,
                            BrowserWindowInterface* target_browser,
                            int to_index,
                            const tab_groups::TabGroupId& group_id);

// Moves a tab from `source_browser` to `target_browser` at `to_index`. If the
// operation results in a split tab group in the destination browser, noop
// instead.
void MoveTabAcrossWindows(
    BrowserWindowInterface* source_browser,
    int from_index,
    BrowserWindowInterface* target_browser,
    int to_index,
    std::optional<tab_groups::TabGroupId> to_group_id = std::nullopt);

}  // namespace tab_strip_ui

#endif  // CUSTOM_BROWSER_UI_WEB_UI_TAB_STRIP_UI_UTIL_H_
