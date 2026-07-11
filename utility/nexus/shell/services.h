// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SERVICES_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SERVICES_H_

#include "mojo/public/cpp/bindings/service_factory.h"

namespace nexus {

// Adds every Nexus utility-process service to `services`. Intended to be
// called from `chrome/utility/services.cc::RegisterMainThreadServices` via
// a small `#if BUILDFLAG(ENABLE_CUSTOM_BROWSER)` guard. The upstream hook
// is added in a later slice; until then this helper is unreferenced and
// purely declarative.
//
// Keeping the registration in the custom_browser tree means upstream's
// services.cc only ever sees a single
// `nexus::RegisterNexusUtilityServices(services)` call, regardless of how
// many Nexus services land later.
void RegisterNexusUtilityServices(mojo::ServiceFactory& services);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SERVICES_H_
