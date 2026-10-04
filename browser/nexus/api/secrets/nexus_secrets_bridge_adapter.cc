// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/secrets/nexus_secrets_bridge_adapter.h"

#include <optional>
#include <string>
#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/secrets/nexus_secret_store.h"

namespace nexus {

NexusSecretsBridgeAdapter::NexusSecretsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    PrefService* prefs)
    : ::nexus::generated::SecretsApiBase(delegate), prefs_(prefs) {}

NexusSecretsBridgeAdapter::~NexusSecretsBridgeAdapter() = default;

void NexusSecretsBridgeAdapter::HandleSetOnUI(
    ::nexus::generated::SecretsSetInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusSecretStore store(prefs_);
  if (std::optional<SecretExpansionError> error =
          store.Set(input.name, input.value,
                    input.origin.value_or(std::string()),
                    input.processEnv.value_or(false))) {
    if (error->code == "INVALID_ARGS") {
      std::move(done).InvalidArgs(std::move(error->message));
    } else {
      std::move(done).Error(std::move(error->code), std::move(error->message));
    }
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusSecretsBridgeAdapter::HandleHasOnUI(
    ::nexus::generated::SecretsHasInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::optional<std::string> origin = NexusSecretStore(prefs_).Has(input.name);
  base::DictValue out;
  out.Set("exists", origin.has_value());
  if (origin.has_value()) {
    out.Set("origin", std::move(*origin));
  }
  std::move(done).Success(std::move(out));
}

void NexusSecretsBridgeAdapter::HandleDeleteOnUI(
    ::nexus::generated::SecretsDeleteInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::DictValue out;
  out.Set("deleted", NexusSecretStore(prefs_).Delete(input.name));
  std::move(done).Success(std::move(out));
}

void NexusSecretsBridgeAdapter::HandleListNamesOnUI(
    ::nexus::generated::SecretsListNamesInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::optional<std::string> name_space;
  if (const std::string* filter = input.payload.FindString("namespace")) {
    name_space = *filter;
  }
  base::ListValue secrets;
  for (const NexusSecretStore::Entry& entry :
       NexusSecretStore(prefs_).ListNames(name_space)) {
    base::DictValue row;
    row.Set("name", entry.name);
    row.Set("origin", entry.origin);
    row.Set("processEnv", entry.process_env);
    secrets.Append(std::move(row));
  }
  base::DictValue out;
  out.Set("secrets", std::move(secrets));
  std::move(done).Success(std::move(out));
}

}  // namespace nexus
