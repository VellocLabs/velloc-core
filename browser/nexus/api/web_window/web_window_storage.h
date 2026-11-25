// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_STORAGE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_STORAGE_H_

#include <string_view>

#include "base/functional/callback_forward.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/storage_partition_config.h"

class Profile;

namespace content {
class WebContents;
}

namespace nexus {

// A kept session's name, as webWindow.open and deleteStoragePartition take
// it: 8 to 32 lower-case ASCII letters.
bool IsWebWindowStorageName(std::string_view name);

// Security: the session a single-web window (F-29) runs in instead of the
// profile's own: a storage partition of the profile, so no cookie or site
// data of the profile is in it. The profile's saved passwords, autofill and
// extensions are not partitioned and still apply. A fresh one is in memory,
// made for one window and cleared when let go; a kept one is on disk and the
// same each time its name is opened. Contract: UI thread; in use while a
// reference is held, and while any tab it was kept with lives.
class WebWindowStorage : public base::RefCounted<WebWindowStorage> {
 public:
  static scoped_refptr<WebWindowStorage> Fresh(Profile* profile);
  // Contract: IsWebWindowStorageName(name).
  static scoped_refptr<WebWindowStorage> Kept(Profile* profile,
                                              std::string_view name);

  WebWindowStorage(const WebWindowStorage&) = delete;
  WebWindowStorage& operator=(const WebWindowStorage&) = delete;

  const content::StoragePartitionConfig& config() const { return config_; }

  // Holds this session for as long as `contents` lives, and as long as any
  // tab or popup it opens in this session lives.
  void KeepWith(content::WebContents* contents);

 private:
  friend class base::RefCounted<WebWindowStorage>;

  WebWindowStorage(Profile* profile, content::StoragePartitionConfig config);
  ~WebWindowStorage();

  base::WeakPtr<Profile> profile_;
  const content::StoragePartitionConfig config_;
};

// `opener`, a tab kept with a session, opened `popup`: a popup in the same
// partition holds that session too. Anything else is left alone.
void KeepWebWindowStorageForPopup(content::WebContents* opener,
                                  content::WebContents* popup);

// Whether the kept session `name` is held (a window or popup is open in it).
bool IsWebWindowStorageInUse(Profile* profile, std::string_view name);

// Whether a DeleteWebWindowStorage of `name` has not answered yet.
bool IsWebWindowStorageDeleting(Profile* profile, std::string_view name);

// Deletes everything the kept session `name` stored, then runs `done`.
// Contract: IsWebWindowStorageName(name), neither in use nor deleting.
void DeleteWebWindowStorage(Profile* profile,
                            std::string_view name,
                            base::OnceClosure done);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_STORAGE_H_
