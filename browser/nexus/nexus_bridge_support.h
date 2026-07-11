#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_SUPPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_SUPPORT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "third_party/blink/public/common/messaging/string_message_codec.h"

namespace nexus::bridge_support {

std::string SerializeJson(base::Value value);

blink::WebMessagePayload Utf8Payload(std::string_view utf8);
std::optional<std::string> PayloadToUtf8(
    const blink::WebMessagePayload& payload);
bool IsBinaryPayload(const blink::WebMessagePayload& payload);
std::vector<uint8_t> PayloadToBytes(const blink::WebMessagePayload& payload);
blink::WebMessagePayload BigBufferToPayload(mojo_base::BigBuffer buffer);

std::vector<uint8_t> ValueListToBytes(const base::ListValue* list);
std::optional<std::string> FindNonEmptyString(const base::DictValue& dict,
                                              std::string_view key);
std::optional<int64_t> FindIntegralValue(const base::DictValue& dict,
                                         std::string_view key);
bool CopyRequiredString(const base::DictValue& dict,
                        std::string_view key,
                        std::string* out,
                        std::string* error_message);

}  // namespace nexus::bridge_support

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_SUPPORT_H_
