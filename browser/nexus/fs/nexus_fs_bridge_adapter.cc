#include "custom_browser/browser/nexus/fs/nexus_fs_bridge_adapter.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "custom_browser/browser/nexus/fs/nexus_fs_host.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_message_port_endpoint.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;
using List = base::ListValue;

Value BuildOkResult() {
  Dict result;
  result.Set("ok", true);
  return Value(std::move(result));
}

}  // namespace

NexusFsBridgeAdapter::NexusFsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    const base::FilePath& cwd)
    : NexusBridgeCapabilityHandler(delegate),
      fs_host_(std::make_unique<NexusFsHost>(cwd)) {
  Register("cwd", &NexusFsBridgeAdapter::HandleCwd);
  Register("stat", "path", &NexusFsBridgeAdapter::HandleStat);
  Register("readdir", "path", &NexusFsBridgeAdapter::HandleReaddir);
  Register("readFile", "path", &NexusFsBridgeAdapter::HandleReadFile);
  Register("readFileBytes", "path", &NexusFsBridgeAdapter::HandleReadFileBytes);
  Register("readRange", "path", &NexusFsBridgeAdapter::HandleReadRange);
  Register("writeFile", "path", &NexusFsBridgeAdapter::HandleWriteFile);
  Register("appendFile", "path", &NexusFsBridgeAdapter::HandleAppendFile);
  Register("mkdir", "path", &NexusFsBridgeAdapter::HandleMkdir);
  Register("rm", "path", &NexusFsBridgeAdapter::HandleRm);
  Register("unlink", "path", &NexusFsBridgeAdapter::HandleUnlink);
  Register("exists", "path", &NexusFsBridgeAdapter::HandleExists);
}

NexusFsBridgeAdapter::~NexusFsBridgeAdapter() = default;

void NexusFsBridgeAdapter::RemovePendingBinaryRequestPort(
    const std::string& request_id) {
  pending_binary_request_ports_.erase(request_id);
}

void NexusFsBridgeAdapter::RemovePendingBinaryRequestPortSoon(
    const std::string& request_id) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&NexusFsBridgeAdapter::RemovePendingBinaryRequestPort,
                     weak_factory_.GetWeakPtr(), request_id));
}

void NexusFsBridgeAdapter::HandleCwd(
    const std::string& id,
    const Dict& route,
    const Dict& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  fs_host_->Cwd(base::BindOnce(
      [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id, Dict route,
         const std::string& cwd) {
        if (!self) {
          return;
        }
        self->SendSuccessResponse(id, route, Value(cwd));
      },
      weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleStat(
    const std::string& id,
    const Dict& route,
    std::string path,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  fs_host_->Stat(
      path,
      base::BindOnce(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
             Dict route, bool success, const std::string& json_stat,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      error_message.value_or("Stat failed"));
              return;
            }

            std::optional<Value> stat_value =
                base::JSONReader::Read(json_stat, base::JSON_PARSE_RFC);
            if (!stat_value.has_value()) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      "Failed to decode stat response");
              return;
            }

            self->SendSuccessResponse(id, route, std::move(*stat_value));
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleReaddir(
    const std::string& id,
    const Dict& route,
    std::string path,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  fs_host_->Readdir(
      path,
      base::BindOnce(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
             Dict route, bool success, const std::string& json_dirents,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      error_message.value_or("Readdir failed"));
              return;
            }

            std::optional<Value> dirents =
                base::JSONReader::Read(json_dirents, base::JSON_PARSE_RFC);
            if (!dirents.has_value()) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      "Failed to decode readdir response");
              return;
            }

            self->SendSuccessResponse(id, route, std::move(*dirents));
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleReadFile(
    const std::string& id,
    const Dict& route,
    std::string path,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  fs_host_->ReadFile(
      path, base::BindOnce(
                [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
                   Dict route, bool success,
                   std::optional<mojo_base::BigBuffer> data,
                   std::optional<std::string> error_message) {
                  if (!self) {
                    return;
                  }
                  if (!success) {
                    self->SendErrorResponse(
                        id, route, "FS_ERROR",
                        error_message.value_or("ReadFile failed"));
                    return;
                  }

                  self->SendBinaryResponse(id, route, Value(),
                                           data.has_value()
                                               ? std::move(*data)
                                               : mojo_base::BigBuffer(),
                                           "$result");
                },
                weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleReadFileBytes(
    const std::string& id,
    const Dict& route,
    std::string path,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  const int64_t max_bytes = std::max<int64_t>(
      0, bridge_support::FindIntegralValue(payload, "maxBytes").value_or(0));
  if (max_bytes > 0) {
    fs_host_->ReadRange(
        path, 0, base::checked_cast<uint32_t>(max_bytes),
        base::BindOnce(
            [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
               Dict route, bool success,
               std::optional<mojo_base::BigBuffer> data,
               uint32_t /*bytes_read*/,
               std::optional<std::string> error_message) {
              if (!self) {
                return;
              }
              if (!success) {
                self->SendErrorResponse(
                    id, route, "FS_ERROR",
                    error_message.value_or("ReadFileBytes failed"));
                return;
              }

              self->SendBinaryResponse(
                  id, route, Value(),
                  data.has_value() ? std::move(*data) : mojo_base::BigBuffer(),
                  "$result");
            },
            weak_factory_.GetWeakPtr(), id, route.Clone()));
    return;
  }

  fs_host_->ReadFile(
      path, base::BindOnce(
                [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
                   Dict route, bool success,
                   std::optional<mojo_base::BigBuffer> data,
                   std::optional<std::string> error_message) {
                  if (!self) {
                    return;
                  }
                  if (!success) {
                    self->SendErrorResponse(
                        id, route, "FS_ERROR",
                        error_message.value_or("ReadFileBytes failed"));
                    return;
                  }

                  self->SendBinaryResponse(id, route, Value(),
                                           data.has_value()
                                               ? std::move(*data)
                                               : mojo_base::BigBuffer(),
                                           "$result");
                },
                weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleReadRange(
    const std::string& id,
    const Dict& route,
    std::string path,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  const uint64_t offset = base::checked_cast<uint64_t>(std::max<int64_t>(
      0, bridge_support::FindIntegralValue(payload, "offset").value_or(0)));
  const uint32_t length = base::checked_cast<uint32_t>(std::max<int64_t>(
      0, bridge_support::FindIntegralValue(payload, "length").value_or(0)));

  fs_host_->ReadRange(
      path, offset, length,
      base::BindOnce(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
             Dict route, bool success, std::optional<mojo_base::BigBuffer> data,
             uint32_t bytes_read,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      error_message.value_or("ReadRange failed"));
              return;
            }

            Dict result;
            result.Set("bytesRead", static_cast<int>(bytes_read));
            self->SendBinaryResponse(
                id, route, Value(std::move(result)),
                data.has_value() ? std::move(*data) : mojo_base::BigBuffer(),
                "data");
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleWriteFile(
    const std::string& id,
    const Dict& route,
    std::string path,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> ports) {
  const List* content = payload.FindList("content");
  if (content) {
    fs_host_->WriteFile(
        path, mojo_base::BigBuffer(bridge_support::ValueListToBytes(content)),
        base::BindOnce(
            [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
               Dict route, bool success,
               std::optional<std::string> error_message) {
              if (!self) {
                return;
              }
              if (!success) {
                self->SendErrorResponse(id, route, "FS_ERROR",
                                        error_message.value_or("WriteFile failed"));
                return;
              }

              self->SendSuccessResponse(id, route, BuildOkResult());
            },
            weak_factory_.GetWeakPtr(), id, route.Clone()));
    return;
  }

  if (ports.empty()) {
    SendInvalidArgsResponse(id, route, "Missing binary field \"content\"");
    return;
  }

  auto route_copy = std::make_shared<Dict>(route.Clone());
  auto payload_received = std::make_shared<bool>(false);
  auto endpoint = std::make_unique<NexusMessagePortEndpoint>(
      std::move(ports.front()),
      base::BindRepeating(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string request_id,
             std::string path, std::shared_ptr<Dict> route,
             std::shared_ptr<bool> payload_received,
             blink::WebMessagePayload payload,
             std::vector<blink::MessagePortDescriptor> nested_ports) {
            if (!self) {
              return;
            }

            if (!nested_ports.empty()) {
              LOG(WARNING) << "[NexusBridgePort] Ignoring nested transferred "
                           << "ports for " << request_id;
            }

            if (!bridge_support::IsBinaryPayload(payload)) {
              self->RemovePendingBinaryRequestPortSoon(request_id);
              self->SendInvalidArgsResponse(
                  request_id, *route,
                  "Binary data port payload must be an ArrayBuffer");
              return;
            }

            *payload_received = true;
            std::vector<uint8_t> bytes = bridge_support::PayloadToBytes(payload);
            self->fs_host_->WriteFile(
                path, mojo_base::BigBuffer(base::span(bytes)),
                base::BindOnce(
                    [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
                       Dict route, bool success,
                       std::optional<std::string> error_message) {
                      if (!self) {
                        return;
                      }
                      self->RemovePendingBinaryRequestPort(id);
                      if (!success) {
                        self->SendErrorResponse(
                            id, route, "FS_ERROR",
                            error_message.value_or("WriteFile failed"));
                        return;
                      }

                      self->SendSuccessResponse(id, route, BuildOkResult());
                    },
                    self, request_id, route->Clone()));
          },
          weak_factory_.GetWeakPtr(), id, path, route_copy, payload_received),
      base::BindRepeating(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string request_id,
             std::shared_ptr<Dict> route,
             std::shared_ptr<bool> payload_received) {
            if (!self) {
              return;
            }
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE,
                base::BindOnce(
                    [](base::WeakPtr<NexusFsBridgeAdapter> self,
                       std::string request_id, std::shared_ptr<Dict> route,
                       std::shared_ptr<bool> payload_received) {
                      if (!self || *payload_received) {
                        return;
                      }
                      self->RemovePendingBinaryRequestPort(request_id);
                      self->SendErrorResponse(request_id, *route, "FS_ERROR",
                                              "Binary data port disconnected");
                    },
                    self, request_id, route, payload_received));
          },
          weak_factory_.GetWeakPtr(), id, route_copy, payload_received));
  pending_binary_request_ports_[id] = std::move(endpoint);
}

void NexusFsBridgeAdapter::HandleAppendFile(
    const std::string& id,
    const Dict& route,
    std::string path,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> ports) {
  const List* data = payload.FindList("data");
  const uint32_t mode = base::checked_cast<uint32_t>(std::max<int64_t>(
      0, bridge_support::FindIntegralValue(payload, "mode").value_or(0666)));

  if (data) {
    fs_host_->AppendFile(
        path, mojo_base::BigBuffer(bridge_support::ValueListToBytes(data)),
        mode,
        base::BindOnce(
            [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
               Dict route, bool success,
               std::optional<std::string> error_message) {
              if (!self) {
                return;
              }
              if (!success) {
                self->SendErrorResponse(
                    id, route, "FS_ERROR",
                    error_message.value_or("AppendFile failed"));
                return;
              }

              self->SendSuccessResponse(id, route, BuildOkResult());
            },
            weak_factory_.GetWeakPtr(), id, route.Clone()));
    return;
  }

  if (ports.empty()) {
    SendInvalidArgsResponse(id, route, "Missing binary field \"data\"");
    return;
  }

  auto route_copy = std::make_shared<Dict>(route.Clone());
  auto payload_received = std::make_shared<bool>(false);
  auto endpoint = std::make_unique<NexusMessagePortEndpoint>(
      std::move(ports.front()),
      base::BindRepeating(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string request_id,
             std::string path, uint32_t mode, std::shared_ptr<Dict> route,
             std::shared_ptr<bool> payload_received,
             blink::WebMessagePayload payload,
             std::vector<blink::MessagePortDescriptor> nested_ports) {
            if (!self) {
              return;
            }

            if (!nested_ports.empty()) {
              LOG(WARNING) << "[NexusBridgePort] Ignoring nested transferred "
                           << "ports for " << request_id;
            }

            if (!bridge_support::IsBinaryPayload(payload)) {
              self->RemovePendingBinaryRequestPortSoon(request_id);
              self->SendInvalidArgsResponse(
                  request_id, *route,
                  "Binary data port payload must be an ArrayBuffer");
              return;
            }

            *payload_received = true;
            std::vector<uint8_t> bytes = bridge_support::PayloadToBytes(payload);
            self->fs_host_->AppendFile(
                path, mojo_base::BigBuffer(base::span(bytes)), mode,
                base::BindOnce(
                    [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
                       Dict route, bool success,
                       std::optional<std::string> error_message) {
                      if (!self) {
                        return;
                      }
                      self->RemovePendingBinaryRequestPort(id);
                      if (!success) {
                        self->SendErrorResponse(
                            id, route, "FS_ERROR",
                            error_message.value_or("AppendFile failed"));
                        return;
                      }

                      self->SendSuccessResponse(id, route, BuildOkResult());
                    },
                    self, request_id, route->Clone()));
          },
          weak_factory_.GetWeakPtr(), id, path, mode, route_copy,
          payload_received),
      base::BindRepeating(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string request_id,
             std::shared_ptr<Dict> route,
             std::shared_ptr<bool> payload_received) {
            if (!self) {
              return;
            }
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE,
                base::BindOnce(
                    [](base::WeakPtr<NexusFsBridgeAdapter> self,
                       std::string request_id, std::shared_ptr<Dict> route,
                       std::shared_ptr<bool> payload_received) {
                      if (!self || *payload_received) {
                        return;
                      }
                      self->RemovePendingBinaryRequestPort(request_id);
                      self->SendErrorResponse(request_id, *route, "FS_ERROR",
                                              "Binary data port disconnected");
                    },
                    self, request_id, route, payload_received));
          },
          weak_factory_.GetWeakPtr(), id, route_copy, payload_received));
  pending_binary_request_ports_[id] = std::move(endpoint);
}

void NexusFsBridgeAdapter::HandleMkdir(
    const std::string& id,
    const Dict& route,
    std::string path,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  const uint32_t mode = base::checked_cast<uint32_t>(std::max<int64_t>(
      0, bridge_support::FindIntegralValue(payload, "mode").value_or(0777)));

  fs_host_->Mkdir(
      path, mode,
      base::BindOnce(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
             Dict route, bool success,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      error_message.value_or("Mkdir failed"));
              return;
            }

            self->SendSuccessResponse(id, route, BuildOkResult());
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleRm(
    const std::string& id,
    const Dict& route,
    std::string path,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  const bool recursive = payload.FindBool("recursive").value_or(false);
  const bool force = payload.FindBool("force").value_or(false);

  fs_host_->Rm(
      path, recursive, force,
      base::BindOnce(
          [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
             Dict route, bool success,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success) {
              self->SendErrorResponse(id, route, "FS_ERROR",
                                      error_message.value_or("Rm failed"));
              return;
            }

            self->SendSuccessResponse(id, route, BuildOkResult());
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleUnlink(
    const std::string& id,
    const Dict& route,
    std::string path,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  fs_host_->Unlink(
      path, base::BindOnce(
                [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
                   Dict route, bool success,
                   std::optional<std::string> error_message) {
                  if (!self) {
                    return;
                  }
                  if (!success) {
                    self->SendErrorResponse(
                        id, route, "FS_ERROR",
                        error_message.value_or("Unlink failed"));
                    return;
                  }

                  self->SendSuccessResponse(id, route, BuildOkResult());
                },
                weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusFsBridgeAdapter::HandleExists(
    const std::string& id,
    const Dict& route,
    std::string path,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  fs_host_->Exists(
      path, base::BindOnce(
                [](base::WeakPtr<NexusFsBridgeAdapter> self, std::string id,
                   Dict route, bool success, bool exists,
                   std::optional<std::string> error_message) {
                  if (!self) {
                    return;
                  }
                  if (!success && error_message.has_value()) {
                    self->SendErrorResponse(id, route, "FS_ERROR",
                                            *error_message);
                    return;
                  }

                  self->SendSuccessResponse(id, route, Value(exists));
                },
                weak_factory_.GetWeakPtr(), id, route.Clone()));
}

}  // namespace nexus
