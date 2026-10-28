// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_INSTALLER_UNINSTALL_PROTOCOL_H_
#define CUSTOM_BROWSER_INSTALLER_UNINSTALL_PROTOCOL_H_

namespace custom_browser::installer {

// Contract: what setup.exe and `chrome.exe --uninstall` say to each other when
// the browser is running at uninstall time.
//
// chrome.exe, finding another instance running, asks the user whether to close
// it. On "close" it exits with CHROME_RESULT_CODE_NORMAL_EXIT_EXP1; setup.exe
// then closes the install's processes and runs chrome.exe --uninstall again
// with kUninstallCloseFailedSwitch, so a browser that is still running is
// reported as "could not close" and offers Try again. Cancel exits as
// CHROME_RESULT_CODE_UNINSTALL_USER_CANCEL, a cancelled uninstall.
inline constexpr char kUninstallCloseFailedSwitch[] =
    "velloc-uninstall-close-failed";

}  // namespace custom_browser::installer

#endif  // CUSTOM_BROWSER_INSTALLER_UNINSTALL_PROTOCOL_H_
