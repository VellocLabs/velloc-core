// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_INSTALLER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_INSTALLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace nexus {

enum class EnsureBinaryStatus {
  // The binary was already present under ~/.velloc/bin.
  kPresent,
  // The binary was just downloaded, verified, and written.
  kInstalled,
  // No pinned asset exists for this host platform/arch.
  kUnsupported,
  // Download / integrity / extraction / write failed.
  kFailed,
};

struct EnsureBinaryResult {
  EnsureBinaryStatus status = EnsureBinaryStatus::kFailed;
  // Absolute path of the managed executable. Set for kPresent/kInstalled,
  // empty otherwise.
  base::FilePath path;
  // Human-readable failure reason (the URL that was tried, the failing
  // step, …) for kFailed/kUnsupported — surfaced in the Grep tool_result
  // and host log so a failed auto-download is diagnosable instead of
  // looking identical to "ripgrep never installed".
  std::string detail;

  bool ok() const {
    return status == EnsureBinaryStatus::kPresent ||
           status == EnsureBinaryStatus::kInstalled;
  }
};

// Lazily downloads a pinned binary into ~/.velloc/bin/<id>/ on first use. The
// download is async; verification, extraction and the write run on a
// ThreadPool MayBlock sequence, never UI. Process-wide singleton: concurrent
// calls share one in-flight download; successes are cached, failures are not.
class NexusBinaryInstaller {
 public:
  using EnsureCallback = base::OnceCallback<void(const EnsureBinaryResult&)>;

  static NexusBinaryInstaller* GetInstance();

  NexusBinaryInstaller(const NexusBinaryInstaller&) = delete;
  NexusBinaryInstaller& operator=(const NexusBinaryInstaller&) = delete;

  // Ensure binary `id` (e.g. "rg") is present, downloading on first use.
  // `callback` runs on the calling sequence once the outcome is known.
  void EnsureBinary(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      const std::string& id,
      EnsureCallback callback);

 private:
  friend class base::NoDestructor<NexusBinaryInstaller>;

  struct PendingInstall;

  NexusBinaryInstaller();
  ~NexusBinaryInstaller();

  void OnBundledChecked(
      std::string id,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      EnsureCallback callback,
      base::FilePath bundled);
  void OnUpToDateChecked(std::string id, bool up_to_date);
  void OnDownloaded(std::string id, std::optional<std::string> body);
  void OnInstalled(std::string id, bool success);
  void Finish(const std::string& id, EnsureBinaryResult result);

  SEQUENCE_CHECKER(sequence_checker_);
  std::map<std::string, std::unique_ptr<PendingInstall>> in_flight_;
  std::map<std::string, EnsureBinaryResult> cached_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_INSTALLER_H_
