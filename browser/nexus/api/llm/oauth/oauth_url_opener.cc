// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_url_opener.h"

#include <utility>

#include "base/no_destructor.h"

namespace nexus {

namespace {

NexusAuthUrlOpener& GetOpener() {
  static base::NoDestructor<NexusAuthUrlOpener> opener;
  return *opener;
}

}  // namespace

void SetNexusAuthUrlOpener(NexusAuthUrlOpener opener) {
  GetOpener() = std::move(opener);
}

void OpenNexusAuthUrl(Profile* profile, const GURL& url) {
  if (GetOpener()) {
    GetOpener().Run(profile, url);
  }
}

}  // namespace nexus
