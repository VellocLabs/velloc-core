// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_UI_NEXUS_UI_BROADCAST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_UI_NEXUS_UI_BROADCAST_H_

#include <string_view>

#include "base/values.h"

class Profile;

namespace nexus {

// Broadcasts a `cr.webUIListenerCallback(event_name)` call (no payload) to
// every open browser window owned by `profile` whose
// `CustomBrowserWebuiView` hosts a live WebUI. Used by the nexus capability
// handlers to push UI-bound signals to the parent WebUI page; the JS side
// registers via `addWebUiListener(event_name, ...)`.
void BroadcastWebUiListenerEventToProfile(Profile* profile,
                                          std::string_view event_name);

// Same as above but also forwards `payload` as the listener's argument:
//   cr.webUIListenerCallback(event_name, payload).
void BroadcastWebUiListenerEventToProfile(Profile* profile,
                                          std::string_view event_name,
                                          base::Value payload);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_UI_NEXUS_UI_BROADCAST_H_
