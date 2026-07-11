#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PERMISSIONS_NEXUS_PERMISSION_MANAGER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PERMISSIONS_NEXUS_PERMISSION_MANAGER_H_

#include <map>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/sequence_checker.h"
#include "url/origin.h"

namespace nexus {

// Permission status for a Nexus capability tier on a specific domain.
enum class PermissionStatus {
  kGranted,
  kDenied,
  kNotAsked,
};

// Nexus-specific capability tiers that require per-domain permission grants.
// Browser-automation permissions are handled by Glic's existing permission
// model (see R-002); these cover ADDITIONAL Nexus-only capabilities.
enum class NexusCapability {
  kFileSystem,
  kClipboard,
  kBrowserData,
  kNotifications,
};

// Converts a capability string (e.g. "file-system") to its enum value.
// Returns nullopt if the string is not a valid capability.
std::optional<NexusCapability> ParseCapability(const std::string& capability);

// Converts a capability enum to its string representation.
std::string CapabilityToString(NexusCapability capability);

// Manages per-domain permission grants for Nexus-specific capability tiers.
//
// Each website origin that uses the Nexus panel gets independent permission
// grants (FR-019a) — granting file-system access to origin A does not affect
// origin B.
//
// Permission state follows data-model.md PermissionGrant transitions:
//   NOT_ASKED → GRANTED (user approves prompt)
//   NOT_ASKED → DENIED  (user rejects prompt)
//   GRANTED   → DENIED  (user revokes in settings)
//   DENIED    → GRANTED (user re-grants in settings)
//
// Thread safety: all methods must be called on the same sequence.
class NexusPermissionManager {
 public:
  using PermissionCallback = base::OnceCallback<void(bool granted)>;

  NexusPermissionManager();
  ~NexusPermissionManager();

  NexusPermissionManager(const NexusPermissionManager&) = delete;
  NexusPermissionManager& operator=(const NexusPermissionManager&) = delete;

  // Checks the current permission status for |capability| on |origin|.
  PermissionStatus CheckPermission(const url::Origin& origin,
                                   NexusCapability capability) const;

  // Requests permission for |capability| on |origin|. If the permission is
  // already granted or denied, invokes |callback| immediately. If the
  // permission is NOT_ASKED, triggers a permission prompt and invokes
  // |callback| with the user's decision.
  void RequestPermission(const url::Origin& origin,
                         NexusCapability capability,
                         PermissionCallback callback);

  // Revokes a previously granted permission.
  void RevokePermission(const url::Origin& origin,
                        NexusCapability capability);

  // Grants a permission directly (for use by settings UI).
  void GrantPermission(const url::Origin& origin,
                       NexusCapability capability);

  // Returns all permission grants for a given origin.
  std::map<NexusCapability, PermissionStatus> GetPermissions(
      const url::Origin& origin) const;

  // Clears all permissions for an origin (used when deleting a domain sandbox).
  void ClearPermissions(const url::Origin& origin);

 private:
  // Composite key for the permission map: (origin, capability).
  using PermissionKey = std::pair<url::Origin, NexusCapability>;

  std::map<PermissionKey, PermissionStatus> permissions_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PERMISSIONS_NEXUS_PERMISSION_MANAGER_H_
