// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_GUEST_REQUEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_GUEST_REQUEST_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "url/gurl.h"

namespace nexus {

// Pure checks over what a guest asks a `network.*` action to send, shared by
// httpGet / httpRequest / streamOpen / wsOpen so the surfaces cannot drift.
// A leaf (//base, //net, //url) so the lite test binary links it.

// Why the header `name: value` may not be sent, or nullopt when it may.
// `net::HttpRequestHeaders::SetHeader` CHECKs both the name (an HTTP token)
// and the value (no CR / LF / NUL), so a header that skips this check takes
// the whole browser process down. Run it on the value that will be sent,
// i.e. after `{{secret:NAME}}` / `{{oauth:NAME}}` expansion.
std::optional<std::string> InvalidGuestHeader(std::string_view name,
                                              std::string_view value);

// The request URL as the guest may see it. A `{{secret:NAME}}` or
// `{{oauth:NAME}}` placeholder in a query value is expanded into the URL that
// goes on the wire; the URL handed back (`finalUrl`) shows the placeholder
// again, so a secret the guest has no read access to is never echoed to it.
class GuestVisibleUrl {
 public:
  GuestVisibleUrl();
  GuestVisibleUrl(const GuestVisibleUrl&);
  GuestVisibleUrl& operator=(const GuestVisibleUrl&);
  GuestVisibleUrl(GuestVisibleUrl&&);
  GuestVisibleUrl& operator=(GuestVisibleUrl&&);
  ~GuestVisibleUrl();

  // `sent` is the URL on the wire, `guest` the same URL with every expanded
  // query value written back as the guest's placeholder text.
  void SetUrls(GURL sent, GURL guest);
  // Records that the guest's `placeholder` value went out as `expanded`.
  void AddExpansion(std::string_view placeholder, std::string_view expanded);

  // `final_url` (where the load ended) as the guest may see it. Unchanged
  // when nothing was expanded. The sent URL itself maps to the guest URL; a
  // URL a redirect produced has each expanded value's query encodings (`+`
  // and `%20` for spaces) replaced by the placeholder's.
  std::string For(const GURL& final_url) const;

 private:
  GURL sent_;
  GURL guest_;
  // Encoded expanded value -> encoded placeholder, longest first.
  std::vector<std::pair<std::string, std::string>> replacements_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_GUEST_REQUEST_H_
