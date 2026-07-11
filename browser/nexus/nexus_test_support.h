// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TEST_SUPPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TEST_SUPPORT_H_

#include "chrome/test/base/testing_profile.h"

namespace nexus::test {

// Disables Glic's eager BrowserContextKeyedService for a TestingProfile.
//
// custom_browser hardcodes GlicEnabling::IsEnabledByFlags() to true (Nexus is
// built on Glic), so every regular profile is glic-eligible. GlicKeyedService
// is "created with browser context" (eager), so GlicKeyedServiceFactory builds
// it during TestingProfile init — and its constructor dereferences
// g_browser_process->profile_manager(), which is null in unit-test binaries
// that don't stand up a TestingProfileManager. That faults (access violation)
// and crashes the whole profile build. Overriding the factory to build no
// service skips it.
//
// Any test that builds a regular TestingProfile but doesn't exercise Glic
// should apply one of these:
//   - AddGlicDisablingFactory(builder) before TestingProfile::Builder::Build().
//   - return GlicDisablingTestingFactories() from a
//     ChromeRenderViewHostTestHarness::GetTestingFactories() override.
void AddGlicDisablingFactory(TestingProfile::Builder& builder);
TestingProfile::TestingFactories GlicDisablingTestingFactories();

}  // namespace nexus::test

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_TEST_SUPPORT_H_
