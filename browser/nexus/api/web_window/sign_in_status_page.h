// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_STATUS_PAGE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_STATUS_PAGE_H_

#include <string>
#include <string_view>

#include "base/time/time.h"

class GURL;

namespace nexus {

// The host's own pages for the last steps of a sign-in in a single-web
// window (F-29): no browser state, so the lite test binary covers them.

enum class SignInStatus {
  // The redirect was captured; the token exchange is running.
  kFinishing,
  // Signed in; the window closes itself after kSignInSuccessHold.
  kSucceeded,
  // The sign-in failed; the window stays open.
  kFailed,
};

// How long the success page shows, counted from its load, before its window
// closes. The page's countdown bar runs for the same time.
inline constexpr base::TimeDelta kSignInSuccessHold = base::Milliseconds(1500);

// The longest failure reason a page shows, in UTF-16 units.
inline constexpr size_t kSignInFailureReasonMaxLength = 300;

// The page text, already localized by the caller.
struct SignInStatusText {
  std::u16string finishing;     // "Finishing sign-in…"
  std::u16string succeeded;     // "You're signed in"
  std::u16string closing;       // "This window will close automatically."
  std::u16string failed;        // "Couldn't sign in"
  std::u16string failed_hint;   // "Close this window and try again in …"
};

// The page for `status` as a data: URL. `reason` is shown on the kFailed
// page only.
// Security: `reason` comes from the network or the runtime; it is
// whitespace-collapsed, cut to kSignInFailureReasonMaxLength and
// HTML-escaped, and the page runs no script.
GURL MakeSignInStatusPage(SignInStatus status,
                          const SignInStatusText& text,
                          std::u16string_view reason = {});

// What a failed OAuth sign-in's `error` code means to the user. The page
// shows a localized line per kind, never the code itself.
enum class SignInFailureKind {
  // The user declined, or the service refused the grant (`access_denied`).
  kDenied,
  // A device code ran out before it was approved (`expired_token`).
  kExpired,
  // Any other code.
  kServiceError,
};

SignInFailureKind ClassifyOAuthSignInError(std::string_view error);

// `reason` as the kFailed page shows it: whitespace runs collapsed, trimmed,
// cut to kSignInFailureReasonMaxLength (an ellipsis marks the cut).
std::u16string NormalizeSignInFailureReason(std::u16string_view reason);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_STATUS_PAGE_H_
