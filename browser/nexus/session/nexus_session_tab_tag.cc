// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"

#include <utility>

#include "content/public/browser/web_contents.h"

namespace nexus {

NexusSessionTabTag::NexusSessionTabTag(content::WebContents* contents,
                                       std::string session_key)
    : content::WebContentsUserData<NexusSessionTabTag>(*contents),
      session_key_(std::move(session_key)) {}

NexusSessionTabTag::~NexusSessionTabTag() = default;

// static
void NexusSessionTabTag::AttachTo(content::WebContents* contents,
                                   std::string session_key) {
  if (!contents) {
    return;
  }
  NexusSessionTabTag* existing = FromWebContents(contents);
  if (existing) {
    if (existing->session_key_ == session_key) {
      return;
    }
    // Re-binding a tab to a different session is unusual but defined:
    // drop the prior tag and create a fresh one so SessionKeyFor stays
    // single-valued.
    contents->RemoveUserData(UserDataKey());
  }
  CreateForWebContents(contents, std::move(session_key));
}

// static
const std::string* NexusSessionTabTag::SessionKeyFor(
    content::WebContents* contents) {
  if (!contents) {
    return nullptr;
  }
  NexusSessionTabTag* tag = FromWebContents(contents);
  if (!tag) {
    return nullptr;
  }
  return &tag->session_key_;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(NexusSessionTabTag);

}  // namespace nexus
