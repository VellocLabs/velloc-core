// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "url/gurl.h"

namespace nexus {

namespace {

// Light and dark follow the window's colour scheme (prefers-color-scheme).
constexpr char kStyle[] =
    ":root{color-scheme:light dark;--fg:#1f1f1f;--muted:#5f6368;"
    "--ok:#188038;--bad:#c5221f;--track:#e3e3e3;--bg:#fff}"
    "@media (prefers-color-scheme:dark){:root{--fg:#e3e3e3;--muted:#9aa0a6;"
    "--ok:#81c995;--bad:#f28b82;--track:#3c4043;--bg:#202124}}"
    "html,body{height:100%;margin:0}"
    "body{display:flex;align-items:center;justify-content:center;"
    "background:var(--bg);color:var(--fg);"
    "font:14px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif}"
    "main{max-width:360px;padding:24px;text-align:center}"
    "h1{font-size:18px;font-weight:600;margin:16px 0 4px}"
    "p{margin:4px 0;color:var(--muted)}"
    ".reason{color:var(--fg);overflow-wrap:anywhere}"
    ".icon{width:48px;height:48px;margin:0 auto}"
    ".spin{width:36px;height:36px;margin:6px auto;border-radius:50%;"
    "border:3px solid var(--track);border-top-color:var(--muted);"
    "animation:spin .9s linear infinite}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".bar{height:3px;margin:20px auto 0;width:160px;border-radius:2px;"
    "background:var(--track);overflow:hidden}"
    ".bar>i{display:block;height:100%;background:var(--ok);"
    "transform-origin:left;animation:drain var(--hold) linear forwards}"
    "@keyframes drain{from{transform:scaleX(1)}to{transform:scaleX(0)}}"
    "@media (prefers-reduced-motion:reduce){.spin,.bar>i{animation:none}}";

constexpr char kCheckIcon[] =
    "<svg class=icon viewBox='0 0 48 48' aria-hidden=true>"
    "<circle cx=24 cy=24 r=22 fill=none stroke='currentColor' "
    "stroke-width=3 style='color:var(--ok)'/>"
    "<path d='M14 25l7 7 13-15' fill=none stroke='currentColor' "
    "stroke-width=3.5 stroke-linecap=round stroke-linejoin=round "
    "style='color:var(--ok)'/></svg>";

constexpr char kCrossIcon[] =
    "<svg class=icon viewBox='0 0 48 48' aria-hidden=true>"
    "<circle cx=24 cy=24 r=22 fill=none stroke='currentColor' "
    "stroke-width=3 style='color:var(--bad)'/>"
    "<path d='M17 17l14 14M31 17L17 31' fill=none stroke='currentColor' "
    "stroke-width=3.5 stroke-linecap=round style='color:var(--bad)'/></svg>";

std::string Escape(std::u16string_view text) {
  return base::EscapeForHTML(base::UTF16ToUTF8(text));
}

}  // namespace

SignInFailureKind ClassifyOAuthSignInError(std::string_view error) {
  if (error == "access_denied") {
    return SignInFailureKind::kDenied;
  }
  if (error == "expired_token") {
    return SignInFailureKind::kExpired;
  }
  return SignInFailureKind::kServiceError;
}

std::u16string NormalizeSignInFailureReason(std::u16string_view reason) {
  std::u16string collapsed =
      base::CollapseWhitespace(std::u16string(reason),
                               /*trim_sequences_with_line_breaks=*/false);
  if (collapsed.size() <= kSignInFailureReasonMaxLength) {
    return collapsed;
  }
  collapsed.resize(kSignInFailureReasonMaxLength - 1);
  // Never leave half a surrogate pair before the ellipsis.
  if (!collapsed.empty() && collapsed.back() >= 0xD800 &&
      collapsed.back() <= 0xDBFF) {
    collapsed.pop_back();
  }
  return collapsed + u"…";
}

GURL MakeSignInStatusPage(SignInStatus status,
                          const SignInStatusText& text,
                          std::u16string_view reason) {
  std::string title;
  std::string body;
  switch (status) {
    case SignInStatus::kFinishing:
      title = Escape(text.finishing);
      body = base::StrCat({"<div class=spin role=progressbar></div><h1>",
                           title, "</h1>"});
      break;
    case SignInStatus::kSucceeded:
      title = Escape(text.succeeded);
      body = base::StrCat(
          {kCheckIcon, "<h1>", title, "</h1><p>", Escape(text.closing),
           "</p><div class=bar style='--hold:",
           base::NumberToString(kSignInSuccessHold.InMilliseconds()),
           "ms'><i></i></div>"});
      break;
    case SignInStatus::kFailed: {
      title = Escape(text.failed);
      const std::u16string shown = NormalizeSignInFailureReason(reason);
      body = base::StrCat({kCrossIcon, "<h1>", title, "</h1>"});
      if (!shown.empty()) {
        base::StrAppend(&body, {"<p class=reason>", Escape(shown), "</p>"});
      }
      base::StrAppend(&body, {"<p>", Escape(text.failed_hint), "</p>"});
      break;
    }
  }
  const std::string html = base::StrCat(
      {"<!doctype html><meta charset=utf-8>"
       "<meta http-equiv=Content-Security-Policy content=\"default-src "
       "'none'; style-src 'unsafe-inline'\"><title>",
       title, "</title><style>", kStyle, "</style><main role=status>", body,
       "</main>"});
  return GURL(base::StrCat(
      {"data:text/html;charset=utf-8,",
       base::EscapeUrlEncodedData(html, /*use_plus=*/false)}));
}

}  // namespace nexus
