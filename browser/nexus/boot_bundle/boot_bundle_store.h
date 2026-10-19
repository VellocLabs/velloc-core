// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_STORE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_STORE_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_index.h"

namespace nexus {

// Contract: owns the loaded guest boot bundle (F-33). Lives on the UI
// thread; file reads run on the thread pool. Until Load() completes, or when
// the bundle is absent or malformed, index() is null and nothing is served.
class BootBundleStore {
 public:
  using ReadCallback = base::OnceCallback<void(std::optional<std::string>)>;

  // The browser-process store, or null before SetGlobal().
  static BootBundleStore* Get();
  static void SetGlobal(BootBundleStore* store);

  BootBundleStore();
  BootBundleStore(const BootBundleStore&) = delete;
  BootBundleStore& operator=(const BootBundleStore&) = delete;
  ~BootBundleStore();

  // Reads `<dir>/nexus_boot_bundle.json` and validates it against
  // `<dir>/nexus_boot_bundle.dat`, off the UI thread.
  void Load(const base::FilePath& dir);

  // Test seam: installs an already-parsed index and its data file.
  void SetForTesting(BootBundleIndex index, const base::FilePath& data_path);

  const BootBundleIndex* index() const;

  // Reads one entry's bytes; nullopt when the read fails or comes up short.
  void Read(const BootBundleEntry& entry, ReadCallback callback) const;

 private:
  void OnLoaded(std::optional<BootBundleIndex> index);

  SEQUENCE_CHECKER(sequence_checker_);
  base::FilePath data_path_;
  std::optional<BootBundleIndex> index_;
  base::WeakPtrFactory<BootBundleStore> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_STORE_H_
