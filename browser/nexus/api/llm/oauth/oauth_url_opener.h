// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_URL_OPENER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_URL_OPENER_H_

#include "base/functional/callback.h"

class GURL;
class Profile;

namespace nexus {

// Opens an OAuth authorization URL in a normal browser tab.
using NexusAuthUrlOpener = base::RepeatingCallback<void(Profile*, const GURL&)>;

// Registers the opener. Set once at startup by the browser-UI layer, which is
// the only layer allowed to depend on //chrome/browser/ui (and thus on
// custom_browser::window_util::OpenUrlInNormalWindow). The settings WebUI
// handler cannot depend on //chrome/browser/ui without forming a GN cycle
// (//chrome/browser/ui already depends on the settings handler), so it reaches
// the helper through this indirection instead.
void SetNexusAuthUrlOpener(NexusAuthUrlOpener opener);

// Opens `url` for `profile` via the registered opener. No-op if none was
// registered.
void OpenNexusAuthUrl(Profile* profile, const GURL& url);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_OAUTH_OAUTH_URL_OPENER_H_
