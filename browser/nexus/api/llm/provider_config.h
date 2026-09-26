// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_

#include <string>

#include "base/containers/flat_map.h"
#include "base/values.h"

class PrefService;

namespace nexus {

// Returns a pointer to the stored Dict for |provider_id| inside
// prefs::kNexusProviderConfigs, or nullptr if no entry exists or |prefs| is
// null. The returned pointer is owned by the PrefService and remains valid
// for the lifetime of the underlying stored dict; callers that need an
// independent copy should call ``Clone()``.
//
// Previously duplicated between nexus_llm_host.cc and
// nexus_settings_handler.cc; consolidated here so both read the same
// provider-config shape from prefs.
const base::DictValue* GetStoredProviderConfig(PrefService* prefs,
                                               const std::string& provider_id);

// Extracts the persisted default model id from a provider config Dict.
// Tries "default_model" first, falling back to the legacy "model" key.
// Returns an empty string if neither is present.
std::string GetStoredDefaultModelId(const base::DictValue& config);

// Seals a settings.json `apiKey` for the `nexus.provider_configs` pref cache.
// A plaintext key becomes `"NEXUSENC1:" + base64(ciphertext)`; a value that is
// already sealed (the guest persists ciphertext after `model.encryptSecret`)
// or empty is returned unchanged, so the pref never holds a plaintext key even
// when an older guest still publishes one. If the OS encryptor is not ready
// yet this returns the input UNCHANGED (plaintext stays plaintext); the
// caller (`LlmBroker::SetProviderConfigs`) closes that window by re-applying
// the same configs from `NexusSecretCrypt::WhenReady`, which re-seals them.
// If the encryptor IS ready and the OS encrypt still fails, nothing re-tries,
// so this returns "" (logged) rather than letting the plaintext reach the
// pref; settings.json keeps the key and republishes it on the next boot.
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

// Stores |user_agent| as the provider config's pinned User-Agent (the
// `user_agent` key `LlmProvider::ConfigureUserAgent` reads). Null or empty
// REMOVES the key, so the provider falls back to the host default. The guest
// sends `userAgent` on every `publishProviderConfigs` ('' when the provider
// has no preset), and the broker merges a publish onto the stored config key
// by key — keeping an absent or empty value would leave a previous preset's
// User-Agent on the wire.
void SetStoredUserAgent(const std::string* user_agent, base::DictValue& config);

// True when the STORED provider config carries something the user supplied:
// a credential, an endpoint, a model choice, or the custom-provider marker —
// the "did the user set this provider up" half of the picker gate.
//
// `LlmProvider::IsConfigured()` cannot answer it alone — that reports whether
// the provider is USABLE, a much weaker claim: every keyless provider (Ollama)
// is usable the moment it has a default base URL. Gating on IsConfigured()
// alone puts Ollama in every user's picker on a profile that has never heard
// of it, hands it the catalog's default selection, and suppresses the
// first-run provider-missing dialog (all three caught by the e2e suite).
//
// `base_url` counts when PRESENT, even empty: '' is what a save that left the
// field on the default stores (the default itself is guest-owned and never
// written into the pref), so for a keyless provider it can be the only
// evidence of setup. Stream-guard overrides are deliberately NOT evidence:
// `stream_idle_timeout_s` / `max_concurrent_streams` are pushed for every
// curated provider by the guest's boot-time `publishProviderConfigsAtBoot`,
// so a provider the user never touched still ends up with a
// (stream-guard-only) pref entry.
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
