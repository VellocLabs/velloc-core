// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_installer_channel.h"

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"

namespace nexus {

namespace {

// A channel name is a short word; anything longer is not a marker we wrote.
constexpr size_t kMaxMarkerBytes = 64;

struct PendingMarker {
  base::FilePath path;
  std::string channel;
};

std::optional<PendingMarker>& Pending() {
  static base::NoDestructor<std::optional<PendingMarker>> pending;
  return *pending;
}

}  // namespace

std::optional<std::string> WebappUrlForInstallerChannel(
    std::string_view channel) {
  const std::string name =
      base::ToLowerASCII(base::TrimWhitespaceASCII(channel, base::TRIM_ALL));
  if (name == "beta") {
    return std::string(kBetaChannelWebappUrl);
  }
  return std::nullopt;
}

void LoadPendingInstallerChannel(const base::FilePath& user_data_dir) {
  const base::FilePath path = user_data_dir.Append(kInstallerChannelFileName);
  if (!base::PathExists(path)) {
    return;
  }
  std::string channel;
  base::ReadFileToStringWithMaxSize(path, &channel, kMaxMarkerBytes);
  Pending() = PendingMarker{path, std::move(channel)};
}

bool ApplyPendingInstallerChannel(PrefService* prefs) {
  if (!prefs || !Pending()) {
    return false;
  }
  const PendingMarker marker = std::move(*Pending());
  Pending().reset();

  const std::optional<std::string> url =
      WebappUrlForInstallerChannel(marker.channel);
  if (url) {
    prefs->SetString(prefs::kNexusWebappUrl, *url);
  }
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
      base::BindOnce(base::IgnoreResult(&base::DeleteFile), marker.path));
  return url.has_value();
}

}  // namespace nexus
