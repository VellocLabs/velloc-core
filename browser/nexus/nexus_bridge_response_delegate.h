#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_RESPONSE_DELEGATE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_RESPONSE_DELEGATE_H_

#include <string>
#include <string_view>

#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

class NexusBridgeResponseDelegate {
 public:
  virtual ~NexusBridgeResponseDelegate() = default;

  virtual void SendSuccessResponse(const std::string& id,
                                   const base::DictValue& route,
                                   base::Value result) = 0;
  virtual void SendBinaryResponse(const std::string& id,
                                  const base::DictValue& route,
                                  base::Value result,
                                  mojo_base::BigBuffer data,
                                  std::string_view result_binary_field) = 0;
  virtual void SendErrorResponse(const std::string& id,
                                 const base::DictValue& route,
                                 const std::string& code,
                                 const std::string& message) = 0;

  // Host-initiated event push (no client request to correlate with). The
  // envelope mirrors a normal bridge response except `direction: "event"`
  // and the `id` field is host-assigned (callers usually pass an empty
  // string; the transport will fill in a synthetic id). Used by the
  // LLM bridge to deliver `session.applySelection` and `session.pickFailed`
  // events from the WebUI quick-settings popup down to nexus_web.
  virtual void SendEvent(const base::DictValue& route, base::Value payload) = 0;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_RESPONSE_DELEGATE_H_
