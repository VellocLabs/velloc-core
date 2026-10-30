// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_

#include <string>

#include "base/containers/flat_map.h"
#include "base/values.h"

class PrefService;

namespace nexus {

// Returns the stored Dict for |provider_id| in prefs::kNexusProviderConfigs,
// or nullptr if absent or |prefs| is null. Owned by the PrefService; call
// ``Clone()`` for an independent copy.
const base::DictValue* GetStoredProviderConfig(PrefService* prefs,
                                               const std::string& provider_id);

// Extracts the persisted default model id from a provider config Dict.
// Tries "default_model" first, falling back to the legacy "model" key.
// Returns an empty string if neither is present.
std::string GetStoredDefaultModelId(const base::DictValue& config);

// Seals a settings.json `apiKey` for the pref cache as `"NEXUSENC1:" +
// base64(ciphertext)`; sealed or empty values pass through. If the encryptor
// is not ready the input is returned unchanged (the caller re-applies from
// `NexusSecretCrypt::WhenReady`); if encryption fails it returns "" so no
// plaintext reaches the pref.
std::string SealApiKeyForPref(const std::string& api_key);

// Whether two STORED api_key values name different secrets. Compares the
// decrypted plaintexts, not the stored strings: OS encryption is randomised,
// so the same key sealed twice has two ciphertexts, and a sealed key vs its
// legacy plaintext must still read as "unchanged" — otherwise a boot-time
// re-publish would drop the provider's `last_verification` stamp every start.
// While the encryptor is not ready (Decrypt yields "" for every marked value)
// it falls back to a byte compare.
bool StoredApiKeysDiffer(const std::string& stored_a,
                         const std::string& stored_b);

// Merges settings.json `secrets` (name -> value) into the stored config's
// `secrets` dict, sealing each value like the api key (SealApiKeyForPref).
// MERGED, never replaced: a secret the host captured from a token response
// (model.rawStreamOpen `captureSecrets`) sits in the pref before the guest
// persisted it, so a publish that does not yet carry it must not drop it. An
// empty value removes that one secret. Null |settings_secrets| is a no-op.
// These are the names `{{secret:NAME}}` placeholders resolve against.
void MergeStoredSecrets(const base::DictValue* settings_secrets,
                        base::DictValue& config);

// Stores |user_agent| as the provider config's pinned User-Agent (the
// `user_agent` key `LlmProvider::ConfigureUserAgent` reads). Null or empty
// REMOVES the key, so the provider falls back to the host default. The guest
// sends `userAgent` on every `publishProviderConfigs` ('' when the provider
// has no preset), and the broker merges a publish onto the stored config key
// by key — keeping an absent or empty value would leave a previous preset's
// User-Agent on the wire.
void SetStoredUserAgent(const std::string* user_agent, base::DictValue& config);

// True when the stored config carries something the user supplied (a
// credential, endpoint, model choice or custom-provider marker) — unlike
// IsConfigured(), which keyless providers satisfy by default. `base_url`
// counts when present, even empty; stream-guard overrides do not, since the
// guest pushes them for every curated provider.
bool HasUserProviderSetup(const base::DictValue& config);

// Each provider's default base URL, read from the guest-published model
// catalog (`{providers: [{providerId, defaultBaseUrl, …}]}`). The guest owns
// these defaults — the host compiles none in for API-key vendors — so this
// is the only place a provider learns where to send requests absent a user
// override. Entries without a non-empty `defaultBaseUrl` are skipped.
base::flat_map<std::string, std::string> GetCatalogDefaultBaseUrls(
    const base::DictValue& catalog);

// Cleanup of `nexus.provider_configs` (passed as the whole dict; the broker
// runs it once per profile, gated by kNexusBakedDefaultBaseUrlsCleared):
// blanks a provider's stored `base_url` when it equals the default this
// binary used to compile in, so the provider follows the guest's default
// again. The key is kept as '' ("follow the default"), so the entry still
// reads as set up (HasUserProviderSetup). Returns whether anything changed.
bool ClearBakedDefaultBaseUrls(base::DictValue& provider_configs);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_
