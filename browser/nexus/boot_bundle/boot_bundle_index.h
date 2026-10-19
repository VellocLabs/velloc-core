// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_INDEX_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_INDEX_H_

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace nexus {

// One file of the installer's guest boot bundle: `size` bytes at `offset` in
// the bundle's data file.
struct BootBundleEntry {
  uint64_t offset = 0;
  uint64_t size = 0;
  std::string mime_type;
};

// Contract: the parsed index of the guest boot bundle (F-33), the copy of one
// web runtime build shipped next to chrome.dll. Lookup maps a request URL to
// an entry only for an exact listed path under `base` on a listed https
// origin; nothing is derived from the URL beyond that map key.
class BootBundleIndex {
 public:
  BootBundleIndex();
  BootBundleIndex(BootBundleIndex&&);
  BootBundleIndex& operator=(BootBundleIndex&&);
  ~BootBundleIndex();

  // Parses the index JSON; nullopt when malformed. Entries outside
  // `data_size` are rejected.
  static std::optional<BootBundleIndex> Parse(std::string_view json,
                                              uint64_t data_size);

  // The entry serving `url`, or null. The directory URL `<base>` maps to
  // `index.html`; the query and fragment are ignored.
  const BootBundleEntry* Lookup(const GURL& url) const;

  bool ServesOrigin(const url::Origin& origin) const;
  bool empty() const { return files_.empty(); }

  // The build's time, used as every served response's response_time so the
  // code cache stays valid across boots.
  base::Time build_time() const { return build_time_; }

 private:
  std::string base_;
  std::set<url::Origin> origins_;
  std::map<std::string, BootBundleEntry, std::less<>> files_;
  base::Time build_time_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_INDEX_H_
