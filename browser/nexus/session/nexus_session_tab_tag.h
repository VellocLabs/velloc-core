// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_TAG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_TAG_H_

#include <string>

#include "content/public/browser/web_contents_user_data.h"

namespace content {
class WebContents;
}

namespace nexus {

// Per-tab tag naming the Nexus session that owns the tab (WebContentsUserData).
// The tag, not the tab group, is the source of truth; untagged tabs are
// invisible to every session's filters.
class NexusSessionTabTag
    : public content::WebContentsUserData<NexusSessionTabTag> {
 public:
  NexusSessionTabTag(const NexusSessionTabTag&) = delete;
  NexusSessionTabTag& operator=(const NexusSessionTabTag&) = delete;
  ~NexusSessionTabTag() override;

  // Idempotent: attaches a tag carrying `session_key`. If a tag already
  // exists with a different session_key, replaces it. Calling with the
  // same session_key is a no-op.
  static void AttachTo(content::WebContents* contents,
                       std::string session_key);

  // Returns the session_key of the tag attached to `contents`, or null
  // if the tab has no tag. Returned pointer is owned by the tag and
  // lives as long as `contents`.
  static const std::string* SessionKeyFor(content::WebContents* contents);

  const std::string& session_key() const { return session_key_; }

 private:
  // Constructor reached only through `WebContentsUserData::CreateForWebContents`.
  // Direct callers must use `AttachTo` instead.
  friend class content::WebContentsUserData<NexusSessionTabTag>;
  NexusSessionTabTag(content::WebContents* contents, std::string session_key);

  std::string session_key_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_TAG_H_
