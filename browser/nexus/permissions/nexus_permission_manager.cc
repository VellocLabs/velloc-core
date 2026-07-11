#include "custom_browser/browser/nexus/permissions/nexus_permission_manager.h"

#include <utility>

#include "base/logging.h"

namespace nexus {

std::optional<NexusCapability> ParseCapability(const std::string& capability) {
  if (capability == "file-system")
    return NexusCapability::kFileSystem;
  if (capability == "clipboard")
    return NexusCapability::kClipboard;
  if (capability == "browser-data")
    return NexusCapability::kBrowserData;
  if (capability == "notifications")
    return NexusCapability::kNotifications;
  return std::nullopt;
}

std::string CapabilityToString(NexusCapability capability) {
  switch (capability) {
    case NexusCapability::kFileSystem:
      return "file-system";
    case NexusCapability::kClipboard:
      return "clipboard";
    case NexusCapability::kBrowserData:
      return "browser-data";
    case NexusCapability::kNotifications:
      return "notifications";
  }
}

NexusPermissionManager::NexusPermissionManager() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NexusPermissionManager::~NexusPermissionManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

PermissionStatus NexusPermissionManager::CheckPermission(
    const url::Origin& origin,
    NexusCapability capability) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = permissions_.find({origin, capability});
  if (it == permissions_.end())
    return PermissionStatus::kNotAsked;
  return it->second;
}

void NexusPermissionManager::RequestPermission(
    const url::Origin& origin,
    NexusCapability capability,
    PermissionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  PermissionStatus status = CheckPermission(origin, capability);
  switch (status) {
    case PermissionStatus::kGranted:
      std::move(callback).Run(/*granted=*/true);
      return;
    case PermissionStatus::kDenied:
      std::move(callback).Run(/*granted=*/false);
      return;
    case PermissionStatus::kNotAsked:
      break;
  }

  // TODO(nexus): Show a Chromium permission prompt following the standard
  // permission request UX (similar to camera, microphone, location). For now,
  // auto-grant to unblock development. This will be replaced with a proper
  // permission bubble UI in a follow-up task.
  DVLOG(1) << "Auto-granting permission for " << CapabilityToString(capability)
           << " to " << origin;
  permissions_[{origin, capability}] = PermissionStatus::kGranted;
  std::move(callback).Run(/*granted=*/true);
}

void NexusPermissionManager::RevokePermission(const url::Origin& origin,
                                              NexusCapability capability) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  permissions_[{origin, capability}] = PermissionStatus::kDenied;
  DVLOG(1) << "Revoked permission for " << CapabilityToString(capability)
           << " from " << origin;
}

void NexusPermissionManager::GrantPermission(const url::Origin& origin,
                                             NexusCapability capability) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  permissions_[{origin, capability}] = PermissionStatus::kGranted;
  DVLOG(1) << "Granted permission for " << CapabilityToString(capability)
           << " to " << origin;
}

std::map<NexusCapability, PermissionStatus>
NexusPermissionManager::GetPermissions(const url::Origin& origin) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::map<NexusCapability, PermissionStatus> result;
  for (const auto& [key, status] : permissions_) {
    if (key.first == origin) {
      result[key.second] = status;
    }
  }
  return result;
}

void NexusPermissionManager::ClearPermissions(const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  for (auto it = permissions_.begin(); it != permissions_.end();) {
    if (it->first.first == origin) {
      it = permissions_.erase(it);
    } else {
      ++it;
    }
  }
  DVLOG(1) << "Cleared all permissions for " << origin;
}

}  // namespace nexus
