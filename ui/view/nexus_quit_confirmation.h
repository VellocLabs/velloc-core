// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UI_VIEW_NEXUS_QUIT_CONFIRMATION_H_
#define CUSTOM_BROWSER_UI_VIEW_NEXUS_QUIT_CONFIRMATION_H_

#include "base/functional/callback_forward.h"

namespace custom_browser {

// Runs `quit` — synchronously when no remote-access device is connected (the
// caller's behaviour is then exactly what it was), otherwise after the user
// answers "Yes" to a dialog saying quitting disconnects them. "No" drops it.
//
// Backs the two explicit quit actions: the tray menu's "Quit" (through
// NexusLauncherDelegate::ConfirmQuit) and the app menu's Exit (the guarded
// hook in chrome::Exit()). Closing a window is not a quit and never asks.
//
// One dialog at a time: a request made while one is showing is dropped — the
// user's answer to the open one decides.
void ConfirmQuitWithRemoteDevices(base::OnceClosure quit);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_NEXUS_QUIT_CONFIRMATION_H_
