// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SERVICES_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SERVICES_H_

#include "mojo/public/cpp/bindings/service_factory.h"

namespace nexus {

// Adds every Nexus utility-process service to `services`, for upstream
// services.cc to call behind an ENABLE_CUSTOM_BROWSER guard.
void RegisterNexusUtilityServices(mojo::ServiceFactory& services);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_SERVICES_H_
