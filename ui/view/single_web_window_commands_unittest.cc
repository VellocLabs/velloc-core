// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/ui/view/single_web_window_commands.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser {
namespace {

TEST(SingleWebWindowCommandsTest, PageCommandsStayOn) {
  for (int command : {IDC_BACK, IDC_FORWARD, IDC_RELOAD, IDC_STOP, IDC_FIND,
                      IDC_ZOOM_PLUS, IDC_PRINT, IDC_DEV_TOOLS, IDC_CLOSE_TAB,
                      IDC_CLOSE_WINDOW, IDC_COPY, IDC_PASTE, IDC_FULLSCREEN}) {
    EXPECT_TRUE(IsCommandAllowedInSingleWebWindow(command)) << command;
  }
}

TEST(SingleWebWindowCommandsTest, CommandsThatAddTabsOrEditTheAddressAreOff) {
  for (int command :
       {IDC_NEW_TAB, IDC_NEW_TAB_TO_RIGHT, IDC_RESTORE_TAB, IDC_DUPLICATE_TAB,
        IDC_SELECT_NEXT_TAB, IDC_FOCUS_LOCATION, IDC_FOCUS_SEARCH,
        IDC_FOCUS_TOOLBAR, IDC_OPEN_FILE, IDC_SHOW_HISTORY,
        IDC_SHOW_DOWNLOADS, IDC_SHOW_BOOKMARK_MANAGER, IDC_OPTIONS,
        IDC_VIEW_SOURCE, IDC_TAB_SEARCH, IDC_SHOW_APP_MENU,
        IDC_CLEAR_BROWSING_DATA}) {
    EXPECT_FALSE(IsCommandAllowedInSingleWebWindow(command)) << command;
  }
}

TEST(SingleWebWindowCommandsTest, SystemMenuCommandsStayOn) {
  for (int command :
       {IDC_NEW_WINDOW, IDC_TASK_MANAGER_CONTEXT_MENU, IDC_CLOSE_WINDOW}) {
    EXPECT_TRUE(IsCommandAllowedInSingleWebWindow(command)) << command;
  }
}

#if BUILDFLAG(IS_WIN)
TEST(SingleWebWindowCommandsTest, AppCommandsThatAddTabsOrGoHomeAreOff) {
  for (int app : {APPCOMMAND_NEW, APPCOMMAND_BROWSER_HOME, APPCOMMAND_OPEN,
                  APPCOMMAND_BROWSER_SEARCH, APPCOMMAND_HELP}) {
    EXPECT_FALSE(
        IsCommandAllowedInSingleWebWindow(BrowserCommandForAppCommand(app)))
        << app;
  }
  for (int app : {APPCOMMAND_BROWSER_BACKWARD, APPCOMMAND_BROWSER_FORWARD,
                  APPCOMMAND_BROWSER_REFRESH, APPCOMMAND_BROWSER_STOP,
                  APPCOMMAND_PRINT, APPCOMMAND_COPY}) {
    EXPECT_TRUE(
        IsCommandAllowedInSingleWebWindow(BrowserCommandForAppCommand(app)))
        << app;
  }
}
#endif

TEST(SingleWebWindowCommandsTest, NonAppCommandIdsAreNotTranslated) {
  EXPECT_EQ(BrowserCommandForAppCommand(IDC_NEW_TAB), -1);
  EXPECT_EQ(BrowserCommandForAppCommand(0xF060 /* SC_CLOSE */), -1);
}

TEST(SingleWebWindowCommandsTest, UnknownCommandsAreOff) {
  EXPECT_FALSE(IsCommandAllowedInSingleWebWindow(0));
  EXPECT_FALSE(IsCommandAllowedInSingleWebWindow(-1));
}

}  // namespace
}  // namespace custom_browser
