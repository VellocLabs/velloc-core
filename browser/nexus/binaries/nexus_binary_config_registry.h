// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_CONFIG_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_CONFIG_REGISTRY_H_

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"

namespace nexus {

// Process-global store for the binary-download registry the guest
// (nexus_web) publishes over the bridge (`model.publishBinaryRegistry`).
// It lets the pinned URL/SHA-256/version set be UPDATED ONLINE — by
// editing the public `managed-binaries.json` nexus_web serves — without
// rebuilding the C++ host. When empty (no valid config published yet),
// callers fall back to the compiled-in registry, so the feature always
// works offline.
//
// Security: this store dictates what binary the installer downloads AND
// its expected hash, so it is an RCE control surface. The guest only ever
// populates it from a config fetched SAME-ORIGIN (the origin already runs
// the guest JS), so it adds no new trust root; the SHA-256 here remains
// the integrity gate on the actual binary bytes.
//
// Threading: UI-thread affine (the bridge handler that writes it and the
// installer that reads it both run on the UI thread). No locking — guard
// with the sequence checker.
class NexusBinaryConfigRegistry {
 public:
  static NexusBinaryConfigRegistry& Get();

  NexusBinaryConfigRegistry(const NexusBinaryConfigRegistry&) = delete;
  NexusBinaryConfigRegistry& operator=(const NexusBinaryConfigRegistry&) =
      delete;

  // Replace the registry from a guest-supplied dict:
  //   { "binaries": [ {
  //       "id": "rg", "label"?: "...", "version"?: "...",
  //       "assets": [ {
  //         "platform": "win32"|"darwin"|"linux",
  //         "arch": "x64"|"arm64",
  //         "url": "https://...", "sha256": "<64 hex>",
  //         "format": "zip"|"tar.gz", "member": "<path in archive>" } ] } ] }
  //
  // Malformed entries/assets are skipped individually. If the dict
  // yields ZERO valid assets the previous good config is KEPT (a bad
  // publish can't wipe a working registry). Returns the number of assets
  // accepted (0 = nothing changed).
  size_t SetFromDict(const base::DictValue& dict);

  // Resolve the published asset for (id, platform, arch), or nullopt.
  std::optional<ResolvedBinaryAsset> Resolve(std::string_view id,
                                             HostPlatform platform,
                                             HostArch arch) const;

  bool empty() const;
  void ClearForTesting();

 private:
  friend class base::NoDestructor<NexusBinaryConfigRegistry>;

  NexusBinaryConfigRegistry();
  ~NexusBinaryConfigRegistry();

  // Keyed by "<id>|<platform-int>|<arch-int>" so the value is a flat
  // ResolvedBinaryAsset — no nested header struct to special-member.
  // Transparent comparator so string_view-built keys don't double-allocate.
  std::map<std::string, ResolvedBinaryAsset, std::less<>> assets_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_CONFIG_REGISTRY_H_
