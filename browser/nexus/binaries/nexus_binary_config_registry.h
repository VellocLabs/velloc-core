// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

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

// Process-global store for the binary-download registry the guest publishes
// (`model.publishBinaryRegistry`); when empty, callers use the compiled-in one.
// Security: it dictates what the installer downloads and its expected hash.
// The guest fills it only from a same-origin config, so it adds no trust root;
// the SHA-256 stays the integrity gate on the bytes. UI thread only.
class NexusBinaryConfigRegistry {
 public:
  static NexusBinaryConfigRegistry& Get();

  NexusBinaryConfigRegistry(const NexusBinaryConfigRegistry&) = delete;
  NexusBinaryConfigRegistry& operator=(const NexusBinaryConfigRegistry&) =
      delete;

  // Replaces the registry from a guest-supplied dict:
  //   { "binaries": [ { "id", "label"?, "version"?, "assets": [ { "platform",
  //       "arch", "url", "sha256", "format": "zip"|"tar.gz", "member" } ] } ] }
  // Malformed entries are skipped; zero valid assets keeps the previous
  // config. Returns the number of assets accepted (0 = nothing changed).
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
