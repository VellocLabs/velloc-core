// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_storage.h"

#include <map>
#include <utility>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ptr_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/storage_partition.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace nexus {

namespace {

// Partition domains are [a-z]*. A kept session has a domain of its own, so
// deleting it by domain deletes nothing else.
constexpr char kFreshDomain[] = "vellocfresh";
constexpr char kKeptDomainPrefix[] = "vellockept";
constexpr size_t kNameMin = 8;
constexpr size_t kNameMax = 32;

const char kUseKey[] = "nexus.web_window_storage_use";

// How many WebWindowStorage objects hold each partition domain.
struct StorageUse : public base::SupportsUserData::Data {
  std::map<std::string, int> held;
};

StorageUse& UseOf(Profile* profile) {
  auto* use = static_cast<StorageUse*>(profile->GetUserData(kUseKey));
  if (!use) {
    auto owned = std::make_unique<StorageUse>();
    use = owned.get();
    profile->SetUserData(kUseKey, std::move(owned));
  }
  return *use;
}

std::string KeptDomain(std::string_view name) {
  return base::StrCat({kKeptDomainPrefix, name});
}

}  // namespace

bool IsWebWindowStorageName(std::string_view name) {
  if (name.size() < kNameMin || name.size() > kNameMax) {
    return false;
  }
  for (char c : name) {
    if (!base::IsAsciiLower(c)) {
      return false;
    }
  }
  return true;
}

// static
std::unique_ptr<WebWindowStorage> WebWindowStorage::Fresh(Profile* profile) {
  return base::WrapUnique(new WebWindowStorage(
      profile,
      content::StoragePartitionConfig::Create(
          profile, kFreshDomain,
          base::Uuid::GenerateRandomV4().AsLowercaseString(),
          /*in_memory=*/true),
      /*fresh=*/true));
}

// static
std::unique_ptr<WebWindowStorage> WebWindowStorage::Kept(
    Profile* profile,
    std::string_view name) {
  return base::WrapUnique(new WebWindowStorage(
      profile,
      content::StoragePartitionConfig::Create(profile, KeptDomain(name),
                                              /*partition_name=*/"",
                                              /*in_memory=*/false),
      /*fresh=*/false));
}

WebWindowStorage::WebWindowStorage(Profile* profile,
                                   content::StoragePartitionConfig config,
                                   bool fresh)
    : profile_(profile->GetWeakPtr()),
      config_(std::move(config)),
      fresh_(fresh) {
  ++UseOf(profile).held[config_.partition_domain()];
}

WebWindowStorage::~WebWindowStorage() {
  Profile* profile = profile_.get();
  if (!profile) {
    return;
  }
  std::map<std::string, int>& held = UseOf(profile).held;
  auto it = held.find(config_.partition_domain());
  if (it != held.end() && --it->second <= 0) {
    held.erase(it);
  }
  if (!fresh_) {
    return;
  }
  // The partition object lives as long as the profile; what it stored goes.
  if (content::StoragePartition* partition =
          profile->GetStoragePartition(config_, /*can_create=*/false)) {
    partition->ClearData(
        ~content::StoragePartition::REMOVE_DATA_MASK_SHADER_CACHE,
        blink::StorageKey(), base::Time(), base::Time::Max(),
        base::DoNothing());
  }
}

bool IsWebWindowStorageInUse(Profile* profile, std::string_view name) {
  return UseOf(profile).held.contains(KeptDomain(name));
}

void DeleteWebWindowStorage(Profile* profile,
                            std::string_view name,
                            base::OnceClosure done) {
  // A partition loaded in this run is emptied; one that is not has its
  // directory removed.
  profile->AsyncObliterateStoragePartition(KeptDomain(name), base::DoNothing(),
                                           std::move(done));
}

}  // namespace nexus
