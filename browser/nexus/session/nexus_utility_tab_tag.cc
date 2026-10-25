// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_utility_tab_tag.h"

#include <utility>

#include "content/public/browser/web_contents.h"

namespace nexus {

NexusUtilityTabTag::NexusUtilityTabTag(content::WebContents* contents,
                                       std::string key)
    : content::WebContentsUserData<NexusUtilityTabTag>(*contents),
      key_(std::move(key)) {}

NexusUtilityTabTag::~NexusUtilityTabTag() = default;

// static
void NexusUtilityTabTag::AttachTo(content::WebContents* contents,
                                  std::string key) {
  if (!contents) {
    return;
  }
  if (NexusUtilityTabTag* existing = FromWebContents(contents)) {
    if (existing->key_ == key) {
      return;
    }
    contents->RemoveUserData(UserDataKey());
  }
  CreateForWebContents(contents, std::move(key));
}

// static
const std::string* NexusUtilityTabTag::KeyFor(content::WebContents* contents) {
  if (!contents) {
    return nullptr;
  }
  NexusUtilityTabTag* tag = FromWebContents(contents);
  return tag ? &tag->key_ : nullptr;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(NexusUtilityTabTag);

}  // namespace nexus
