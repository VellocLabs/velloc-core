// Compatibility stub. The real header lives at
// `api/ui/nexus_ui_bridge_adapter.h` after the Phase 11h move
// (ui bridge adapter -> api/ui/). Siso's deps DB retains stale
// dependency records on this old path until each .obj that ever
// included it gets recompiled. The stub satisfies the pre-flight
// deps check in the meantime; once the affected .objs rebuild,
// their fresh deps records point at the new path and this stub
// becomes orphaned and can be deleted on the next clean build.

#include "custom_browser/browser/nexus/api/ui/nexus_ui_bridge_adapter.h"
