// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_

// Bridge interface version — the compatibility contract between this
// binary and the nexus_web guest runtime.
//
// The number is a single monotonic integer defined once per side:
//   - C++: NEXUS_INTERFACE_VERSION below
//   - JS:  NEXUS_INTERFACE_VERSION in
//          nexus_web/src/browser/bridge/protocol/interfaceVersion.ts
//
// Bump BOTH (paired commits, web side first) whenever the host<->guest
// contract changes: the *.api.json / *.tool.json declarations, the TS wire
// types under nexus_web/src/browser/bridge/protocol/, the bootstrap message
// shape, or a provider wire dialect. Web-only fixes that keep the contract
// intact do NOT bump. The ninja-time gate
// (scripts/check_interface_version.py, wired in BUILD.gn) fails the build
// if the two constants ever diverge.
//
// Enforcement is URL-based: GetResolvedNexusGuestURL() appends "v<N>/" to
// any https guest base URL, so a release binary can only ever request the
// web build deployed for its own contract; the guest compares that path
// segment against its compiled-in constant and hard-blocks on mismatch.
// Dev (http://localhost) bases pass through unversioned — dev drift is
// caught by the build gate instead. See
// docs/agents/nexus-interface-versioning.md.

// The number lives in exactly this one #define; everything else (the int
// constant, the string used in URL construction, the JS constant checked
// by the build gate) derives from it.
#define NEXUS_INTERFACE_VERSION 1

#define NEXUS_IF_VERSION_STR_IMPL(x) #x
#define NEXUS_IF_VERSION_STR(x) NEXUS_IF_VERSION_STR_IMPL(x)

namespace nexus {

inline constexpr int kNexusInterfaceVersion = NEXUS_INTERFACE_VERSION;
inline constexpr char kNexusInterfaceVersionStr[] =
    NEXUS_IF_VERSION_STR(NEXUS_INTERFACE_VERSION);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_
