// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_COMMON_NEXUS_RUNTIME_PATHS_H_
#define CUSTOM_BROWSER_COMMON_NEXUS_RUNTIME_PATHS_H_

#include <string_view>

#include "base/files/file_path.h"

namespace custom_browser {

// On-disk locations for the Nexus runtime data directory (`~/.velloc`),
// resolved in pure C++ so both the browser-process binary installer and
// the sandboxed utility-process shell policy/resolver compute the SAME
// paths without round-tripping through the WebUI guest.
//
// The runtime dir name is derived from `kCustomBrowserBrandPathComponent`
// ("Velloc" -> ".velloc"), mirroring the guest's
// `RUNTIME_DIR_NAME = '.' + brand.toLowerCase()` so the C++ and TS sides
// agree on a single layout.
//
// Home resolution is env-first (USERPROFILE/HOME, then PathService
// DIR_HOME), matching `NexusFsHost::UserHome` — the env-first order is
// what makes the dirs honour test isolation overrides.

// Absolute path of the user home directory, or an empty FilePath when it
// can't be resolved.
base::FilePath GetNexusUserHomeDir();

// Absolute path of `~/.velloc`. Empty when home can't be resolved.
base::FilePath GetNexusRuntimeDir();

// Absolute path of `~/.velloc/bin` — the root that holds runtime-
// downloaded binaries. Empty when home can't be resolved.
base::FilePath GetNexusManagedBinDir();

// Absolute path of the managed executable for `id`:
// `~/.velloc/bin/<id>/<id>` (with a `.exe` suffix on Windows). The `id`
// is the shell tool id (e.g. "rg"), which doubles as the subdir name and
// the executable basename. Empty when home can't be resolved.
base::FilePath GetManagedBinaryPath(std::string_view id);

// True when `path` resolves to a location inside `~/.velloc/bin/`. Used
// by the shell policy to allow execing an absolute path the installer
// produced (a trusted, C++-set location) even when the cached shell
// capabilities predate the download. Comparison is done on the
// canonicalized (parent) directories so `..` tricks can't escape.
bool IsUnderManagedBinDir(const base::FilePath& path);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_COMMON_NEXUS_RUNTIME_PATHS_H_
