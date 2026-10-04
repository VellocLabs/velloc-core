// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_installer.h"

#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_archive.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"
#include "custom_browser/common/nexus_runtime_paths.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace nexus {

namespace {

// Defence-in-depth cap on a single extracted executable (the SHA check is
// the real gate; this just bounds memory if a pinned member were ever
// huge). 64 MiB clears any CLI binary by orders of magnitude.
constexpr size_t kMaxMemberBytes = 64u * 1024u * 1024u;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_binary_install", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Downloads a pinned, open-source command-line binary (e.g. "
          "ripgrep) that backs a Nexus shell tool, when it is not "
          "already present on the host. The download URL and a SHA-256 "
          "of the archive are hard-coded in the browser; the fetched "
          "bytes are verified against that hash before being written or "
          "executed. No authentication headers or cookies are sent."
        trigger:
          "The model invokes a tool (e.g. Grep, which uses ripgrep) and "
          "the required binary is not installed under ~/.velloc/bin."
        data:
          "None in the request body — this is a GET of a fixed public "
          "release URL."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: NONE
        }
        last_reviewed: "2026-06-18"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Enabled when the corresponding shell tool is enabled in the "
          "AI assistant settings."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

// ThreadPool task: owns the downloaded `body` and delegates to the
// (unit-tested) verify+extract+atomic-write helper. Kept as a thin owner
// so the moved-in string outlives the span the helper reads.
bool InstallToDisk(std::string body,
                   std::string expected_sha256,
                   BinaryArchiveFormat format,
                   std::string member,
                   base::FilePath dest) {
  return VerifyExtractAndWrite(base::as_byte_span(body), expected_sha256,
                               format, member, dest, kMaxMemberBytes);
}

}  // namespace

struct NexusBinaryInstaller::PendingInstall {
  PendingInstall() = default;
  ~PendingInstall() = default;

  scoped_refptr<network::SharedURLLoaderFactory> factory;
  std::string url;
  std::string sha256;
  BinaryArchiveFormat format = BinaryArchiveFormat::kZip;
  std::string member;
  base::FilePath path;
  std::unique_ptr<network::SimpleURLLoader> loader;
  std::vector<EnsureCallback> callbacks;
};

// static
NexusBinaryInstaller* NexusBinaryInstaller::GetInstance() {
  static base::NoDestructor<NexusBinaryInstaller> instance;
  return instance.get();
}

NexusBinaryInstaller::NexusBinaryInstaller() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NexusBinaryInstaller::~NexusBinaryInstaller() = default;

void NexusBinaryInstaller::EnsureBinary(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const std::string& id,
    EnsureCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Cached terminal result (kPresent/kInstalled/kUnsupported) — reply async
  // so the caller never re-enters synchronously.
  auto cached = cached_.find(id);
  if (cached != cached_.end()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), cached->second));
    return;
  }

  // Coalesce onto an in-flight install for the same id.
  auto inflight = in_flight_.find(id);
  if (inflight != in_flight_.end()) {
    inflight->second->callbacks.push_back(std::move(callback));
    return;
  }

  // A packaged build ships the binary next to chrome.dll — use it and never
  // download. The existence check is blocking I/O, so it runs on the pool;
  // in practice the shell resolver already found this copy and the Grep
  // path never gets here.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::FilePath bundled) {
            return !bundled.empty() && base::PathExists(bundled)
                       ? bundled
                       : base::FilePath();
          },
          custom_browser::GetBundledBinaryPath(id)),
      base::BindOnce(&NexusBinaryInstaller::OnBundledChecked,
                     base::Unretained(this), id,
                     std::move(url_loader_factory), std::move(callback)));
}

void NexusBinaryInstaller::OnBundledChecked(
    std::string id,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    EnsureCallback callback,
    base::FilePath bundled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!bundled.empty()) {
    EnsureBinaryResult result{EnsureBinaryStatus::kPresent, bundled};
    cached_[id] = result;
    std::move(callback).Run(result);
    return;
  }
  // A concurrent call may have started the download meanwhile.
  auto inflight = in_flight_.find(id);
  if (inflight != in_flight_.end()) {
    inflight->second->callbacks.push_back(std::move(callback));
    return;
  }

  std::optional<ResolvedBinaryAsset> asset = ResolveBinaryAssetForHost(id);
  base::FilePath path = custom_browser::GetManagedBinaryPath(id);
  if (!asset.has_value() || path.empty()) {
    // No asset for this host (unknown id / unsupported target), or no home
    // dir. Don't cache: a later online publish can add support, and a
    // missing home is transient — both should re-resolve next call. (The
    // re-resolve is a cheap map + table lookup.)
    EnsureBinaryResult result;
    result.status = asset.has_value() ? EnsureBinaryStatus::kFailed
                                      : EnsureBinaryStatus::kUnsupported;
    result.detail = asset.has_value()
                        ? "could not resolve ~/.velloc/bin (no home dir)"
                        : "no pinned asset for this platform/arch";
    LOG(WARNING) << "[nexus] managed-binary '" << id << "' " << result.detail;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), result));
    return;
  }

  auto pending = std::make_unique<PendingInstall>();
  pending->factory = std::move(url_loader_factory);
  pending->url = std::move(asset->url);
  pending->sha256 = std::move(asset->sha256_hex);
  pending->format = asset->format;
  pending->member = std::move(asset->member);
  pending->path = path;
  pending->callbacks.push_back(std::move(callback));
  in_flight_[id] = std::move(pending);

  // "Up to date?" = the binary exists AND its sidecar SHA matches the
  // currently-pinned archive. A version bump (new SHA in the online
  // config) makes this false, so we re-download instead of keeping the
  // stale binary. Blocking I/O → ThreadPool. (BindOnce stores a copy of
  // the sha string and passes it as a string_view at call time.)
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&IsInstalledUpToDate, path, in_flight_[id]->sha256),
      base::BindOnce(&NexusBinaryInstaller::OnUpToDateChecked,
                     base::Unretained(this), id));
}

void NexusBinaryInstaller::OnUpToDateChecked(std::string id, bool up_to_date) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = in_flight_.find(id);
  if (it == in_flight_.end()) {
    return;
  }
  PendingInstall* p = it->second.get();
  if (up_to_date) {
    Finish(id, EnsureBinaryResult{EnsureBinaryStatus::kPresent, p->path});
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(p->url);
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  p->loader =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  p->loader->DownloadToString(
      p->factory.get(),
      base::BindOnce(&NexusBinaryInstaller::OnDownloaded,
                     base::Unretained(this), id),
      network::SimpleURLLoader::kMaxBoundedStringDownloadSize);
}

void NexusBinaryInstaller::OnDownloaded(std::string id,
                                        std::optional<std::string> body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = in_flight_.find(id);
  if (it == in_flight_.end()) {
    return;
  }
  PendingInstall* p = it->second.get();
  if (!body || body->empty()) {
    const std::string detail = "download failed (url=" + p->url + ")";
    LOG(WARNING) << "[nexus] managed-binary '" << id << "' " << detail;
    Finish(id, EnsureBinaryResult{EnsureBinaryStatus::kFailed, base::FilePath(),
                                  detail});
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&InstallToDisk, std::move(*body), p->sha256, p->format,
                     p->member, p->path),
      base::BindOnce(&NexusBinaryInstaller::OnInstalled,
                     base::Unretained(this), id));
}

void NexusBinaryInstaller::OnInstalled(std::string id, bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = in_flight_.find(id);
  if (it == in_flight_.end()) {
    return;
  }
  base::FilePath path = it->second->path;
  if (success) {
    Finish(id, EnsureBinaryResult{EnsureBinaryStatus::kInstalled, path});
    return;
  }
  // The bytes downloaded but SHA-256 verify / archive extraction / write
  // failed — usually a wrong pinned hash or a non-archive response body.
  const std::string detail =
      "downloaded but SHA-256 verify or extraction failed (url=" +
      it->second->url + ")";
  LOG(WARNING) << "[nexus] managed-binary '" << id << "' " << detail;
  Finish(id,
         EnsureBinaryResult{EnsureBinaryStatus::kFailed, base::FilePath(),
                            detail});
}

void NexusBinaryInstaller::Finish(const std::string& id,
                                  EnsureBinaryResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = in_flight_.find(id);
  if (it == in_flight_.end()) {
    return;
  }
  std::vector<EnsureCallback> callbacks = std::move(it->second->callbacks);
  in_flight_.erase(it);

  // Cache only terminal successes / unsupported — never a transient failure.
  if (result.status != EnsureBinaryStatus::kFailed) {
    cached_[id] = result;
  }

  for (EnsureCallback& callback : callbacks) {
    std::move(callback).Run(result);
  }
}

}  // namespace nexus
