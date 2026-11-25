// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_ISOLATED_WINDOW_SESSION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_ISOLATED_WINDOW_SESSION_H_

class BrowserWindowInterface;
class Profile;

namespace nexus {

// The session of a single-web window opened with `isolatedSession` (F-29):
// an off-the-record profile of its own.
//
// Security: nothing of `profile` (cookies, stored passwords, site data) is
// in it, and no two calls return the same one, so two sign-in windows never
// share a signed-in state. Everything it stored goes when it is destroyed.
//
// Contract: UI thread. `profile` is a regular profile. The caller must hand
// the result to DestroyIsolatedWindowSessionAfter(), or it lives as long as
// `profile`.
Profile* CreateIsolatedWindowSession(Profile* profile);

// Destroys `session` once `browser`, its one window, has closed; at once
// when `browser` is null (the window was never created). Outlives the
// caller: a connection that goes away while its window is closing does not
// leak the session.
void DestroyIsolatedWindowSessionAfter(BrowserWindowInterface* browser,
                                       Profile* session);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_ISOLATED_WINDOW_SESSION_H_
