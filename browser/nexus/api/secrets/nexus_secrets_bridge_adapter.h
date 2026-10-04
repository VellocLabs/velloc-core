// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SECRETS_NEXUS_SECRETS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SECRETS_NEXUS_SECRETS_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/secrets/secrets_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class PrefService;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// The `secrets` capability (native-primitives.md): set / has / delete /
// listNames over NexusSecretStore — namespaced, origin-bound secrets sealed
// with the OS keystore. There is no get: a secret's plaintext is only ever
// substituted host-side into a request to its bound origin (the network
// adapter's `{{secret:NAME}}` handling). All actions run on UI (PrefService)
// and answer synchronously.
class NexusSecretsBridgeAdapter : public ::nexus::generated::SecretsApiBase {
 public:
  // `prefs` is the profile's; null (no profile) answers every write
  // SECRET_STORE_UNAVAILABLE and every read as empty.
  NexusSecretsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            PrefService* prefs);
  ~NexusSecretsBridgeAdapter() override;

  NexusSecretsBridgeAdapter(const NexusSecretsBridgeAdapter&) = delete;
  NexusSecretsBridgeAdapter& operator=(const NexusSecretsBridgeAdapter&) =
      delete;

 private:
  void HandleSetOnUI(::nexus::generated::SecretsSetInput input,
                     std::vector<blink::MessagePortDescriptor> ports,
                     ::nexus::BridgeResponseCallback done) override;
  void HandleHasOnUI(::nexus::generated::SecretsHasInput input,
                     std::vector<blink::MessagePortDescriptor> ports,
                     ::nexus::BridgeResponseCallback done) override;
  void HandleDeleteOnUI(::nexus::generated::SecretsDeleteInput input,
                        std::vector<blink::MessagePortDescriptor> ports,
                        ::nexus::BridgeResponseCallback done) override;
  void HandleListNamesOnUI(::nexus::generated::SecretsListNamesInput input,
                           std::vector<blink::MessagePortDescriptor> ports,
                           ::nexus::BridgeResponseCallback done) override;

  raw_ptr<PrefService> prefs_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SECRETS_NEXUS_SECRETS_BRIDGE_ADAPTER_H_
