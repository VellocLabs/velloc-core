// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INSTALLER_CHANNEL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INSTALLER_CHANNEL_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

class PrefService;

namespace nexus {

// The one-shot marker file an installer leaves in the user data dir when it
// installs onto a runtime channel (G-3). Its content is the channel name,
// e.g. "beta".
inline constexpr base::FilePath::CharType kInstallerChannelFileName[] =
    FILE_PATH_LITERAL("Installer Channel");

// The `nexus.webapp_url` value that selects the beta runtime; the same value
// the About page's channel picker writes.
inline constexpr char kBetaChannelWebappUrl[] = "https://beta.velloc.app/";

// The `nexus.webapp_url` value for a channel name (case-insensitive,
// surrounding whitespace ignored), or nullopt for a name that selects
// nothing — only "beta" does.
std::optional<std::string> WebappUrlForInstallerChannel(
    std::string_view channel);

// Reads the marker in `user_data_dir` and holds it for
// ApplyPendingInstallerChannel. Blocking file IO: call once at startup, on
// the main thread, before the UI thread disallows blocking.
void LoadPendingInstallerChannel(const base::FilePath& user_data_dir);

// Applies the held marker to `prefs` once: "beta" sets `nexus.webapp_url` to
// kBetaChannelWebappUrl, overriding the current value. The marker file is
// then deleted on the thread pool, any content, so a later channel switch in
// About sticks until the next beta install. Returns true when the pref was
// written. A null `prefs` keeps the marker pending. UI thread.
bool ApplyPendingInstallerChannel(PrefService* prefs);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INSTALLER_CHANNEL_H_
