#include "custom_browser/browser/nexus/domain/domain_sandbox_manager.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "crypto/sha2.h"

namespace nexus {

namespace {

// Subdirectory under the profile directory for domain sandbox storage.
constexpr char kSandboxStorageDir[] = "AgentDomainSandboxes";

// Creates a filesystem-safe directory name from an origin by hashing it.
std::string OriginToDirectoryName(const url::Origin& origin) {
  std::string hash = crypto::SHA256HashString(origin.Serialize());
  return base::HexEncode(hash).substr(0, 16);
}

}  // namespace

DomainSandbox::DomainSandbox() = default;
DomainSandbox::DomainSandbox(const DomainSandbox&) = default;
DomainSandbox& DomainSandbox::operator=(const DomainSandbox&) = default;
DomainSandbox::~DomainSandbox() = default;

DomainSandboxManager::DomainSandboxManager(
    const base::FilePath& profile_path)
    : profile_path_(profile_path) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  DVLOG(1) << "DomainSandboxManager created for profile: "
           << profile_path.value();
}

DomainSandboxManager::~DomainSandboxManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

DomainSandbox* DomainSandboxManager::GetOrCreateSandbox(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!origin.opaque()) << "Cannot create sandbox for opaque origin";

  auto it = sandboxes_.find(origin);
  if (it != sandboxes_.end()) {
    it->second->last_used = base::Time::Now();
    return it->second.get();
  }

  auto sandbox = std::make_unique<DomainSandbox>();
  sandbox->origin = origin;
  sandbox->first_used = base::Time::Now();
  sandbox->last_used = sandbox->first_used;
  sandbox->storage_path = GetStoragePath(origin);

  DomainSandbox* raw_ptr = sandbox.get();
  sandboxes_[origin] = std::move(sandbox);

  DVLOG(1) << "Created domain sandbox for: " << origin;

  // TODO(nexus): Persist sandbox metadata to LevelDB under
  // profile_path_/AgentDomainSandboxes/. For the foundational phase,
  // sandboxes are in-memory only. LevelDB persistence will be added
  // when session store (T056) and capability handlers (T046-T049) need
  // persistent domain state.

  return raw_ptr;
}

DomainSandbox* DomainSandboxManager::GetActiveSandbox() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (active_domain_.opaque())
    return nullptr;

  auto it = sandboxes_.find(active_domain_);
  if (it == sandboxes_.end())
    return nullptr;

  return it->second.get();
}

url::Origin DomainSandboxManager::GetActiveDomain() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return active_domain_;
}

void DomainSandboxManager::SetActiveDomain(const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (active_domain_ == origin)
    return;

  // Ensure sandbox exists for the new domain.
  GetOrCreateSandbox(origin);
  active_domain_ = origin;
  DVLOG(1) << "Active domain set to: " << origin;
}

void DomainSandboxManager::DeleteSandbox(const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = sandboxes_.find(origin);
  if (it == sandboxes_.end()) {
    DLOG(WARNING) << "Attempted to delete non-existent sandbox: " << origin;
    return;
  }

  sandboxes_.erase(it);

  // If the deleted domain was active, clear the active domain.
  if (active_domain_ == origin)
    active_domain_ = url::Origin();

  DVLOG(1) << "Deleted domain sandbox for: " << origin;

  // TODO(nexus): Delete LevelDB data and any filesystem artifacts for this
  // domain when persistence is added.
}

bool DomainSandboxManager::HasSandbox(const url::Origin& origin) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return sandboxes_.find(origin) != sandboxes_.end();
}

std::vector<url::Origin> DomainSandboxManager::GetAllDomains() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::vector<url::Origin> domains;
  domains.reserve(sandboxes_.size());
  for (const auto& [origin, sandbox] : sandboxes_) {
    domains.push_back(origin);
  }
  return domains;
}

void DomainSandboxManager::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // TODO(nexus): Flush any pending LevelDB writes.
  sandboxes_.clear();
  active_domain_ = url::Origin();
  DVLOG(1) << "DomainSandboxManager shut down";
}

base::FilePath DomainSandboxManager::GetStoragePath(
    const url::Origin& origin) const {
  return profile_path_
      .AppendASCII(kSandboxStorageDir)
      .AppendASCII(OriginToDirectoryName(origin));
}

}  // namespace nexus
