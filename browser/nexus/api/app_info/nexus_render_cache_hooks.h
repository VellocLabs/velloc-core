// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_RENDER_CACHE_HOOKS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_RENDER_CACHE_HOOKS_H_

#include "base/values.h"
#include "custom_browser/browser/nexus/api/app_info/app_info_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace nexus {

// Contract: the host side of `appInfo.purgeRenderCaches`. A build links one
// definition of these two functions. The stub adds no primitive and answers
// NOT_IMPLEMENTED, so the runtime never calls the action. UI thread.

// Appends the primitives this build's purge supports to the host manifest's
// list; the runtime calls the action only when it finds them there.
void AppendRenderCachePrimitives(base::ListValue& primitives);

// Runs the action for `profile` and answers `done` exactly once.
void HandlePurgeRenderCaches(Profile* profile,
                             generated::PurgeRenderCachesInput input,
                             BridgeResponseCallback done);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_RENDER_CACHE_HOOKS_H_
