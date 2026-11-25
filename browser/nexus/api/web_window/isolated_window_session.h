// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_ISOLATED_WINDOW_SESSION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_ISOLATED_WINDOW_SESSION_H_

class Profile;

namespace nexus {

// Security: a fresh off-the-record session of `profile` for one single-web
// window (F-29): none of the profile's cookies or site data, never shared
// with another call. Null when policy disables off-the-record windows.
// Contract: UI thread; pass the result to
// DestroyIsolatedWindowSessionWhenUnused(), or it lives as long as `profile`.
Profile* CreateIsolatedWindowSession(Profile* profile);

// Destroys `session` once no window of it is open, popups included: soon if
// none is open now. Outlives the caller; stops if the session is destroyed
// first (shutdown).
void DestroyIsolatedWindowSessionWhenUnused(Profile* session);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_ISOLATED_WINDOW_SESSION_H_
