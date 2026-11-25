// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_storage.h"

#include <map>
#include <set>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
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

// How many WebWindowStorage objects hold each partition domain, and the
// kept domains being deleted.
struct StorageUse : public base::SupportsUserData::Data {
  std::map<std::string, int> held;
  std::set<std::string> deleting;
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

// Holds a WebWindowStorage for the life of one tab, and passes it on to the
// popups the tab opens: they run in the same partition.
class StorageKeeper : public content::WebContentsUserData<StorageKeeper>,
                      public content::WebContentsObserver {
 public:
  StorageKeeper(const StorageKeeper&) = delete;
  StorageKeeper& operator=(const StorageKeeper&) = delete;
  ~StorageKeeper() override = default;

  WebWindowStorage* storage() const { return storage_.get(); }

 private:
  friend class content::WebContentsUserData<StorageKeeper>;

  StorageKeeper(content::WebContents* contents,
                scoped_refptr<WebWindowStorage> storage)
      : content::WebContentsUserData<StorageKeeper>(*contents),
        content::WebContentsObserver(contents),
        storage_(std::move(storage)) {}

  void DidOpenRequestedURL(content::WebContents* new_contents,
                           content::RenderFrameHost* /*source*/,
                           const GURL& /*url*/,
                           const content::Referrer& /*referrer*/,
                           WindowOpenDisposition /*disposition*/,
                           ui::PageTransition /*transition*/,
                           bool /*started_from_context_menu*/,
                           bool /*renderer_initiated*/) override {
    KeepWebWindowStorageForPopup(web_contents(), new_contents);
  }

  scoped_refptr<WebWindowStorage> storage_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(StorageKeeper);

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
scoped_refptr<WebWindowStorage> WebWindowStorage::Fresh(Profile* profile) {
  return base::WrapRefCounted(new WebWindowStorage(
      profile, content::StoragePartitionConfig::Create(
                   profile, kFreshDomain,
                   base::Uuid::GenerateRandomV4().AsLowercaseString(),
                   /*in_memory=*/true)));
}

// static
scoped_refptr<WebWindowStorage> WebWindowStorage::Kept(Profile* profile,
                                                       std::string_view name) {
  return base::WrapRefCounted(new WebWindowStorage(
      profile,
      content::StoragePartitionConfig::Create(profile, KeptDomain(name),
                                              /*partition_name=*/"",
                                              /*in_memory=*/false)));
}

WebWindowStorage::WebWindowStorage(Profile* profile,
                                   content::StoragePartitionConfig config)
    : profile_(profile->GetWeakPtr()), config_(std::move(config)) {
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
  if (!config_.in_memory()) {
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

void WebWindowStorage::KeepWith(content::WebContents* contents) {
  if (!StorageKeeper::FromWebContents(contents)) {
    StorageKeeper::CreateForWebContents(contents,
                                        scoped_refptr<WebWindowStorage>(this));
  }
}

void KeepWebWindowStorageForPopup(content::WebContents* opener,
                                  content::WebContents* popup) {
  StorageKeeper* keeper = StorageKeeper::FromWebContents(opener);
  if (keeper && popup->GetPrimaryMainFrame()->GetStoragePartition() ==
                    opener->GetPrimaryMainFrame()->GetStoragePartition()) {
    keeper->storage()->KeepWith(popup);
  }
}

bool IsWebWindowStorageInUse(Profile* profile, std::string_view name) {
  return UseOf(profile).held.contains(KeptDomain(name));
}

bool IsWebWindowStorageDeleting(Profile* profile, std::string_view name) {
  return UseOf(profile).deleting.contains(KeptDomain(name));
}

void DeleteWebWindowStorage(Profile* profile,
                            std::string_view name,
                            base::OnceClosure done) {
  std::string domain = KeptDomain(name);
  UseOf(profile).deleting.insert(domain);
  // A partition loaded in this run is emptied; one that is not has its
  // directory removed. `done` runs once both have finished.
  profile->AsyncObliterateStoragePartition(
      domain, base::DoNothing(),
      base::BindOnce(
          [](base::WeakPtr<Profile> profile, std::string domain,
             base::OnceClosure done) {
            if (profile) {
              UseOf(profile.get()).deleting.erase(domain);
            }
            std::move(done).Run();
          },
          profile->GetWeakPtr(), domain, std::move(done)));
}

}  // namespace nexus
