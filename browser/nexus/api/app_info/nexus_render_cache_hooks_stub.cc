// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/app_info/nexus_render_cache_hooks.h"

#include <utility>

namespace nexus {

void AppendRenderCachePrimitives(base::ListValue& /*primitives*/) {}

void HandlePurgeRenderCaches(Profile* /*profile*/,
                             generated::PurgeRenderCachesInput /*input*/,
                             BridgeResponseCallback done) {
  std::move(done).Error("NOT_IMPLEMENTED",
                        "appInfo.purgeRenderCaches: not in this build");
}

}  // namespace nexus
