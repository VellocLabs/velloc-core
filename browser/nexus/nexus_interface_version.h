// Copyright 2026 VellocLabs. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_

// Bridge interface version: the host<->guest compatibility contract (G-3). The
// source of truth is NEXUS_INTERFACE_VERSION in custom_browser/VERSION; a ninja
// gate fails the build when nexus_web's generated copy drifts. Bump it on any
// contract change (*.api.json / *.tool.json, the TS wire types, the bootstrap
// message shape, a provider wire dialect). Enforced by URL: https guest bases
// get "v<N>/" appended and the guest hard-blocks on a mismatch.

#include "custom_browser/common/generated/nexus_interface_version.inc"

#define NEXUS_IF_VERSION_STR_IMPL(x) #x
#define NEXUS_IF_VERSION_STR(x) NEXUS_IF_VERSION_STR_IMPL(x)

namespace nexus {

inline constexpr int kNexusInterfaceVersion = NEXUS_INTERFACE_VERSION;
inline constexpr char kNexusInterfaceVersionStr[] =
    NEXUS_IF_VERSION_STR(NEXUS_INTERFACE_VERSION);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INTERFACE_VERSION_H_
