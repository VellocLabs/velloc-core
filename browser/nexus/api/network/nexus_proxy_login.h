// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_LOGIN_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_LOGIN_H_

#include <memory>
#include <optional>

#include "content/public/browser/login_delegate.h"
#include "net/base/auth.h"

class PrefService;

namespace content {
class BrowserContext;
}

namespace nexus {

// Answering the sign-in challenge of the person's own fixed proxy with the
// user name and password they stored in Settings → Network (F-18).

// Contract: the credentials to answer `auth_info` with, or nullopt.
// Security: only a proxy's challenge is answered, never a web server's; only
// the first one, so a refused sign-in is not repeated; and only when the
// challenger is the fixed proxy the user's own setting names, so the password
// cannot be drawn out by another proxy or by a value a policy or switch put
// in force.
std::optional<net::AuthCredentials> StoredProxyChallengeCredentials(
    const PrefService& prefs,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt);

// Contract: a delegate that answers `auth_info` through `*callback`
// (asynchronously, and not at all once destroyed), or null when the stored
// sign-in does not apply. `*callback` is consumed only when a delegate is
// returned. UI thread.
std::unique_ptr<content::LoginDelegate> MaybeAnswerProxyChallenge(
    content::BrowserContext* browser_context,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback* callback);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_LOGIN_H_
