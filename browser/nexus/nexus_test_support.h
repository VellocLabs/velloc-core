// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TEST_SUPPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TEST_SUPPORT_H_

#include "chrome/test/base/testing_profile.h"

namespace nexus::test {

// Disables Glic's eager KeyedService for a TestingProfile: its constructor
// dereferences g_browser_process->profile_manager(), null in unit-test binaries.
// Apply via AddGlicDisablingFactory(builder) or GlicDisablingTestingFactories().
void AddGlicDisablingFactory(TestingProfile::Builder& builder);
TestingProfile::TestingFactories GlicDisablingTestingFactories();

}  // namespace nexus::test

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TEST_SUPPORT_H_
