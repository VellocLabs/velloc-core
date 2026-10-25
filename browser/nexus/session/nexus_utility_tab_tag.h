// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_UTILITY_TAB_TAG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_UTILITY_TAB_TAG_H_

#include <string>

#include "content/public/browser/web_contents_user_data.h"

namespace content {
class WebContents;
}

namespace nexus {

// Per-tab tag naming the guest page a utility tab shows (F-35), by its key
// (`ui.openUtilityTab`). A utility tab belongs to no chat session: it never
// carries a NexusSessionTabTag, so every session's tab filter skips it.
class NexusUtilityTabTag
    : public content::WebContentsUserData<NexusUtilityTabTag> {
 public:
  NexusUtilityTabTag(const NexusUtilityTabTag&) = delete;
  NexusUtilityTabTag& operator=(const NexusUtilityTabTag&) = delete;
  ~NexusUtilityTabTag() override;

  // Attaches a tag carrying `key`; replaces a tag with another key, no-op
  // for the same key.
  static void AttachTo(content::WebContents* contents, std::string key);

  // The key of the tag on `contents`, or null when it has none. Owned by the
  // tag, lives as long as `contents`.
  static const std::string* KeyFor(content::WebContents* contents);

  const std::string& key() const { return key_; }

 private:
  friend class content::WebContentsUserData<NexusUtilityTabTag>;
  NexusUtilityTabTag(content::WebContents* contents, std::string key);

  std::string key_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_UTILITY_TAB_TAG_H_
