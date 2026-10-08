#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"

namespace nexus {

void NexusBridgeResponseDelegate::SendErrorResponse(
    const std::string& id,
    const base::DictValue& route,
    const std::string& code,
    const std::string& message,
    const base::DictValue& /*params*/) {
  // Delegates that do not carry structured detail answer with the plain form.
  SendErrorResponse(id, route, code, message);
}

}  // namespace nexus
