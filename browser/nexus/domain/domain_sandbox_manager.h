#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_DOMAIN_DOMAIN_SANDBOX_MANAGER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_DOMAIN_DOMAIN_SANDBOX_MANAGER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "components/keyed_service/core/keyed_service.h"
#include "url/origin.h"

namespace nexus {

// Represents the isolated data boundary for one agent webapp origin.
// Each domain sandbox stores metadata about the webapp's usage, including
// when it was first/last used, its display name, and the storage path
// for domain-specific data (sessions, permissions, etc.).
//
// See data-model.md entity "DomainSandbox" for the full entity definition.
struct DomainSandbox {
  DomainSandbox();
  DomainSandbox(const DomainSandbox&);
  DomainSandbox& operator=(const DomainSandbox&);
  ~DomainSandbox();

  url::Origin origin;
  std::string display_name;
  base::Time first_used;
  base::Time last_used;
  base::FilePath storage_path;  // Profile-relative path for domain files.
};

// Per-profile KeyedService managing DomainSandbox instances: all data access
// is scoped to the active domain; switching domains shows a clean state and
// switching back restores it. Metadata lives in LevelDB at
// `AgentDomainSandboxes/`. UI sequence only.
class DomainSandboxManager : public KeyedService {
 public:
  explicit DomainSandboxManager(const base::FilePath& profile_path);
  ~DomainSandboxManager() override;

  DomainSandboxManager(const DomainSandboxManager&) = delete;
  DomainSandboxManager& operator=(const DomainSandboxManager&) = delete;

  // Returns the sandbox for |origin|, creating it if it doesn't exist.
  // The returned pointer is valid for the lifetime of this service.
  DomainSandbox* GetOrCreateSandbox(const url::Origin& origin);

  // Returns the currently active sandbox, or nullptr if none is active.
  DomainSandbox* GetActiveSandbox() const;

  // Returns the currently active domain origin, or an opaque origin if none.
  url::Origin GetActiveDomain() const;

  // Sets the active domain. Creates a sandbox for the origin if needed.
  // Switches context so that all subsequent data access is scoped to this
  // domain (FR-018b).
  void SetActiveDomain(const url::Origin& origin);

  // Deletes the sandbox for |origin| and all associated data.
  // If the deleted domain was active, clears the active domain.
  void DeleteSandbox(const url::Origin& origin);

  // Returns true if a sandbox exists for |origin|.
  bool HasSandbox(const url::Origin& origin) const;

  // Returns all known domain origins.
  std::vector<url::Origin> GetAllDomains() const;

  // KeyedService:
  void Shutdown() override;

 private:
  // Generates the storage path for a domain sandbox.
  base::FilePath GetStoragePath(const url::Origin& origin) const;

  base::FilePath profile_path_;
  std::map<url::Origin, std::unique_ptr<DomainSandbox>> sandboxes_;
  url::Origin active_domain_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_DOMAIN_DOMAIN_SANDBOX_MANAGER_H_
