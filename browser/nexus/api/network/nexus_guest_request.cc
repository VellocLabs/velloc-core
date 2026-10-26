// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_guest_request.h"

#include <algorithm>

#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "net/http/http_util.h"

namespace nexus {

std::optional<std::string> InvalidGuestHeader(std::string_view name,
                                              std::string_view value) {
  if (name.empty() || !net::HttpUtil::IsValidHeaderName(name)) {
    return "Header not allowed: " + std::string(name);
  }
  if (!net::HttpUtil::IsValidHeaderValue(value)) {
    return "Invalid header value: " + std::string(name);
  }
  return std::nullopt;
}

std::string EncodeGuestFormBody(const base::DictValue& params) {
  std::string body;
  for (const auto [name, value] : params) {
    if (!value.is_string()) {
      continue;
    }
    if (!body.empty()) {
      body += '&';
    }
    body += base::EscapeQueryParamValue(name, /*use_plus=*/true);
    body += '=';
    body += base::EscapeQueryParamValue(value.GetString(), /*use_plus=*/true);
  }
  return body;
}

GuestVisibleUrl::GuestVisibleUrl() = default;
GuestVisibleUrl::GuestVisibleUrl(const GuestVisibleUrl&) = default;
GuestVisibleUrl& GuestVisibleUrl::operator=(const GuestVisibleUrl&) = default;
GuestVisibleUrl::GuestVisibleUrl(GuestVisibleUrl&&) = default;
GuestVisibleUrl& GuestVisibleUrl::operator=(GuestVisibleUrl&&) = default;
GuestVisibleUrl::~GuestVisibleUrl() = default;

void GuestVisibleUrl::SetUrls(GURL sent, GURL guest) {
  sent_ = std::move(sent);
  guest_ = std::move(guest);
}

void GuestVisibleUrl::AddExpansion(std::string_view placeholder,
                                   std::string_view expanded) {
  if (expanded.empty()) {
    return;  // nothing secret went out
  }
  // `net::AppendQueryParameter` encodes with `+` for a space; a server that
  // re-encodes the query in a Location may use `%20`.
  for (const bool use_plus : {true, false}) {
    std::string from = base::EscapeQueryParamValue(expanded, use_plus);
    std::string to = base::EscapeQueryParamValue(placeholder, use_plus);
    if (std::ranges::find(replacements_,
                          std::make_pair(from, to)) == replacements_.end()) {
      replacements_.emplace_back(std::move(from), std::move(to));
    }
  }
  // Longest first, so a value that contains another is replaced whole.
  std::ranges::stable_sort(replacements_, [](const auto& a, const auto& b) {
    return a.first.size() > b.first.size();
  });
}

std::string GuestVisibleUrl::For(const GURL& final_url) const {
  if (replacements_.empty()) {
    return final_url.spec();
  }
  if (final_url == sent_) {
    return guest_.spec();
  }
  std::string spec = final_url.spec();
  for (const auto& [from, to] : replacements_) {
    base::ReplaceSubstringsAfterOffset(&spec, 0, from, to);
  }
  return spec;
}

}  // namespace nexus
