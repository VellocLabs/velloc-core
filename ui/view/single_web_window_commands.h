// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UI_VIEW_SINGLE_WEB_WINDOW_COMMANDS_H_
#define CUSTOM_BROWSER_UI_VIEW_SINGLE_WEB_WINDOW_COMMANDS_H_

#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

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
    case IDC_TASK_MANAGER_CONTEXT_MENU:
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

// The browser command a Windows app command (APPCOMMAND_*) runs, or -1 for
// any other id. Must match BrowserView's own translation, which runs after
// the single-web filter.
inline int BrowserCommandForAppCommand(int app_command) {
#if BUILDFLAG(IS_WIN)
  switch (app_command) {
    case APPCOMMAND_BROWSER_BACKWARD:
      return IDC_BACK;
    case APPCOMMAND_BROWSER_FORWARD:
      return IDC_FORWARD;
    case APPCOMMAND_BROWSER_REFRESH:
      return IDC_RELOAD;
    case APPCOMMAND_BROWSER_HOME:
      return IDC_HOME;
    case APPCOMMAND_BROWSER_STOP:
      return IDC_STOP;
    case APPCOMMAND_BROWSER_SEARCH:
      return IDC_FOCUS_SEARCH;
    case APPCOMMAND_HELP:
      return IDC_HELP_PAGE_VIA_KEYBOARD;
    case APPCOMMAND_NEW:
      return IDC_NEW_TAB;
    case APPCOMMAND_OPEN:
      return IDC_OPEN_FILE;
    case APPCOMMAND_CLOSE:
      return IDC_CLOSE_TAB;
    case APPCOMMAND_SAVE:
      return IDC_SAVE_PAGE;
    case APPCOMMAND_PRINT:
      return IDC_PRINT;
    case APPCOMMAND_COPY:
      return IDC_COPY;
    case APPCOMMAND_CUT:
      return IDC_CUT;
    case APPCOMMAND_PASTE:
      return IDC_PASTE;
    default:
      return -1;
  }
#else
  return -1;
#endif
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_SINGLE_WEB_WINDOW_COMMANDS_H_
