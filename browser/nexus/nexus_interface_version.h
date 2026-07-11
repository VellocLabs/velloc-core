// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_

// Bridge interface version — the compatibility contract between this
// binary and the nexus_web guest runtime.
//
// SINGLE SOURCE OF TRUTH: the NEXUS_INTERFACE_VERSION key in
// custom_browser/VERSION (alongside CUSTOM_BROWSER_MAJOR/PATCH). It fans
// out as generated artifacts:
//   - the #define included below (gen_product_version.py
//     --output-interface-inc, via //custom_browser/common:
//     generate_product_version),
//   - the committed nexus_web TS constant
//     (nexus_web/src/browser/bridge/protocol/interfaceVersion.gen.ts),
//     regenerated with `python browser/nexus/scripts/
//     check_interface_version.py --sync` — the same script's default
//     (check) mode is the ninja gate that fails any binary build whose
//     sibling nexus_web checkout drifted,
//   - the third field of the product version itself
//     (docs/agents/versioning-installer.md).
//
// Bump it (then run --sync and commit both repos, web side first)
// whenever the host<->guest contract changes: the *.api.json /
// *.tool.json declarations, the TS wire types under
// nexus_web/src/browser/bridge/protocol/, the bootstrap message shape, or
// a provider wire dialect. Web-only fixes that keep the contract intact
// do NOT bump.
//
// Enforcement is URL-based: GetResolvedNexusGuestURL() appends "v<N>/" to
// any https guest base URL, so a release binary can only ever request the
// web build deployed for its own contract; the guest compares that path
// segment against its compiled-in constant and hard-blocks on mismatch.
// Dev (http://localhost) bases pass through unversioned — dev drift is
// caught by the build gate instead. See
// docs/agents/nexus-interface-versioning.md.

#include "custom_browser/common/generated/nexus_interface_version.inc"

#define NEXUS_IF_VERSION_STR_IMPL(x) #x
#define NEXUS_IF_VERSION_STR(x) NEXUS_IF_VERSION_STR_IMPL(x)

namespace nexus {

inline constexpr int kNexusInterfaceVersion = NEXUS_INTERFACE_VERSION;
inline constexpr char kNexusInterfaceVersionStr[] =
    NEXUS_IF_VERSION_STR(NEXUS_INTERFACE_VERSION);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_
