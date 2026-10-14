// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"

namespace nexus {

namespace {
// Owned by the UI layer (a NoDestructor singleton registered at startup), so a
// raw pointer is sufficient here and never dangles for the process lifetime.
NexusLauncherDelegate* g_nexus_launcher_delegate = nullptr;
}  // namespace

void SetNexusLauncherDelegate(NexusLauncherDelegate* delegate) {
  g_nexus_launcher_delegate = delegate;
}

NexusLauncherDelegate* GetNexusLauncherDelegate() {
  return g_nexus_launcher_delegate;
}

}  // namespace nexus
