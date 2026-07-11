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
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_RESPONSE_DELEGATE_H_
