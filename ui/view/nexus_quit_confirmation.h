// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UI_VIEW_NEXUS_QUIT_CONFIRMATION_H_
#define CUSTOM_BROWSER_UI_VIEW_NEXUS_QUIT_CONFIRMATION_H_

#include "base/functional/callback_forward.h"

namespace custom_browser {

// Runs `quit` at once when no remote-access device is connected, else after
// the user confirms a dialog ("No" drops it). Backs the tray "Quit" and the
// app-menu Exit; closing a window never asks. One dialog at a time: a request
// made while one is showing is dropped.
void ConfirmQuitWithRemoteDevices(base::OnceClosure quit);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_NEXUS_QUIT_CONFIRMATION_H_
