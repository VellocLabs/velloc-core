// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_INSTALLER_UNINSTALL_DATA_H_
#define CUSTOM_BROWSER_INSTALLER_UNINSTALL_DATA_H_

namespace custom_browser::installer {

// Deletes the runtime data directory (`~/.velloc`). setup.exe calls it beside
// DeleteUserDataDir when the uninstall deletes the profile. Links are never
// followed: a link inside, or `~/.velloc` itself as a link, is removed as a
// link. Returns true when the directory is gone afterwards (or never existed).
bool DeleteNexusRuntimeData();

}  // namespace custom_browser::installer

#endif  // CUSTOM_BROWSER_INSTALLER_UNINSTALL_DATA_H_
