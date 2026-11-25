// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_STORAGE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_STORAGE_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/storage_partition_config.h"

class Profile;

namespace nexus {

// A kept session's name, as webWindow.open and deleteStoragePartition take
// it: 8 to 32 lower-case ASCII letters.
bool IsWebWindowStorageName(std::string_view name);

// Security: the session a single-web window (F-29) runs in instead of the
// profile's own: a storage partition of the profile, so no cookie or site
// data of the profile is in it. A fresh one is in memory, made for one
// window and cleared when let go; a kept one is on disk and the same each
// time its name is opened. Contract: UI thread; hold it as long as the
// window is open.
class WebWindowStorage {
 public:
  static std::unique_ptr<WebWindowStorage> Fresh(Profile* profile);
  // Contract: IsWebWindowStorageName(name).
  static std::unique_ptr<WebWindowStorage> Kept(Profile* profile,
                                                std::string_view name);

  WebWindowStorage(const WebWindowStorage&) = delete;
  WebWindowStorage& operator=(const WebWindowStorage&) = delete;
  ~WebWindowStorage();

  const content::StoragePartitionConfig& config() const { return config_; }

 private:
  WebWindowStorage(Profile* profile,
                   content::StoragePartitionConfig config,
                   bool fresh);

  base::WeakPtr<Profile> profile_;
  const content::StoragePartitionConfig config_;
  const bool fresh_;
};

// Whether a WebWindowStorage of the kept session `name` is held.
bool IsWebWindowStorageInUse(Profile* profile, std::string_view name);

// Deletes everything the kept session `name` stored, then runs `done`.
// Contract: IsWebWindowStorageName(name) and not in use.
void DeleteWebWindowStorage(Profile* profile,
                            std::string_view name,
                            base::OnceClosure done);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_STORAGE_H_
