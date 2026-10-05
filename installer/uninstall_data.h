// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_INSTALLER_UNINSTALL_DATA_H_
#define CUSTOM_BROWSER_INSTALLER_UNINSTALL_DATA_H_

namespace custom_browser::installer {

// Deletes the Nexus runtime data directory (`~/.velloc`: sessions,
// settings, plugins, skills, mods, managed binaries, caches).
//
// setup.exe calls this from its uninstall, beside DeleteUserDataDir, when
// the uninstall deletes the profile — the user ticked the uninstall
// dialog's "delete all data" box, or the command line carries
// --delete-profile. A plain uninstall keeps both.
//
// The directory is resolved like the browser resolves it
// (GetNexusRuntimeDir: USERPROFILE, then HOME, then DIR_HOME). Links are
// never followed: a junction or symlink inside `~/.velloc` (a project
// checkout, say) is removed as a link and its target is left alone, and
// when `~/.velloc` itself is a link only the link is removed.
//
// Returns true when the directory is gone afterwards, including when there
// was none to begin with.
bool DeleteNexusRuntimeData();

}  // namespace custom_browser::installer

#endif  // CUSTOM_BROWSER_INSTALLER_UNINSTALL_DATA_H_
