// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_search_service_launcher.h"

#include <memory>
#include <utility>

#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"

namespace nexus {

namespace {

class SharedSearchLauncherUserData : public base::SupportsUserData::Data {
 public:
  NexusSearchServiceLauncher launcher;
};

const char kSharedSearchLauncherKey[] = "nexus::SharedSearchLauncher";

}  // namespace

NexusSearchServiceLauncher& GetSharedSearchLauncher(Profile* profile) {
  CHECK(profile);
  auto* data = static_cast<SharedSearchLauncherUserData*>(
      profile->GetUserData(kSharedSearchLauncherKey));
  if (!data) {
    auto owned = std::make_unique<SharedSearchLauncherUserData>();
    data = owned.get();
    profile->SetUserData(kSharedSearchLauncherKey, std::move(owned));
  }
  return data->launcher;
}

NexusSearchServiceLauncher::NexusSearchServiceLauncher()
    : remote_("Nexus Search Service", "NexusSearchServiceLauncher",
              kIdleTimeout) {}

NexusSearchServiceLauncher::~NexusSearchServiceLauncher() = default;

mojo::Remote<mojom::NexusSearchService>&
NexusSearchServiceLauncher::GetService() {
  return remote_.GetService();
}

void NexusSearchServiceLauncher::Shutdown() {
  remote_.Shutdown();
}

base::WeakPtr<NexusSearchServiceLauncher>
NexusSearchServiceLauncher::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

}  // namespace nexus
