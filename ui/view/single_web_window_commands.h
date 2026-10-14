// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UI_VIEW_SINGLE_WEB_WINDOW_COMMANDS_H_
#define CUSTOM_BROWSER_UI_VIEW_SINGLE_WEB_WINDOW_COMMANDS_H_

#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"

namespace custom_browser {

// The browser commands a single-web window (kSingleWeb, F-29) still answers
// from the keyboard. Everything else is swallowed: the window holds exactly
// one tab and has no editable address, so a command that adds a tab, focuses
// the hidden omnibox or opens a browser page in this window must not run.
// An allowlist, so a command added upstream stays off until reviewed.
inline bool IsCommandAllowedInSingleWebWindow(int command_id) {
  switch (command_id) {
    case IDC_BACK:
    case IDC_FORWARD:
    case IDC_RELOAD:
    case IDC_RELOAD_BYPASSING_CACHE:
    case IDC_RELOAD_CLEARING_CACHE:
    case IDC_STOP:
    case IDC_ZOOM_PLUS:
    case IDC_ZOOM_NORMAL:
    case IDC_ZOOM_MINUS:
    case IDC_FIND:
    case IDC_FIND_NEXT:
    case IDC_FIND_PREVIOUS:
    case IDC_PRINT:
    case IDC_BASIC_PRINT:
    case IDC_SAVE_PAGE:
    case IDC_DEV_TOOLS:
    case IDC_DEV_TOOLS_CONSOLE:
    case IDC_DEV_TOOLS_INSPECT:
    case IDC_DEV_TOOLS_TOGGLE:
    case IDC_FULLSCREEN:
    case IDC_CLOSE_TAB:
    case IDC_CLOSE_WINDOW:
    case IDC_EXIT:
    case IDC_COPY:
    case IDC_CUT:
    case IDC_PASTE:
    case IDC_NEW_WINDOW:
    case IDC_NEW_INCOGNITO_WINDOW:
    case IDC_TASK_MANAGER:
    case IDC_TASK_MANAGER_SHORTCUT:
    case IDC_MINIMIZE_WINDOW:
    case IDC_MAXIMIZE_WINDOW:
#if BUILDFLAG(IS_LINUX)
    case IDC_RESTORE_WINDOW:
#endif
    case IDC_WINDOW_MENU:
      return true;
    default:
      return false;
  }
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_SINGLE_WEB_WINDOW_COMMANDS_H_
