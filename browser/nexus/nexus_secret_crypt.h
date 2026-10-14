// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_SECRET_CRYPT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_SECRET_CRYPT_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace base {
template <typename T>
class NoDestructor;
}

namespace os_crypt_async {
class OSCryptAsync;
}

namespace nexus {

// Process-wide at-rest encryption for Nexus secrets (provider api_keys, OAuth
// tokens, MCP `env` / `Authorization` values) over the OS-backed
// `os_crypt_async::Encryptor`, behind a synchronous facade. Acquisition is
// async: `EnsureAcquired` starts it; register `WhenReady` to re-read once ready.
// Encrypted values are `"NEXUSENC1:" + base64(ciphertext)`; `Decrypt` returns
// unmarked (plaintext) values unchanged. Browser process only; the MCP utility
// process never sees ciphertext.
class NexusSecretCrypt {
 public:
  static NexusSecretCrypt* GetInstance();

  NexusSecretCrypt(const NexusSecretCrypt&) = delete;
  NexusSecretCrypt& operator=(const NexusSecretCrypt&) = delete;

  // Begin async acquisition of the OS encryptor if not already started /
  // done. Safe to call repeatedly + from any Nexus subsystem; pass
  // `g_browser_process->os_crypt_async()`.
  void EnsureAcquired(os_crypt_async::OSCryptAsync* service);

  bool ready() const { return encryptor_.has_value(); }

  // Runs `callback` once the encryptor is available — immediately if it
  // already is.
  void WhenReady(base::OnceClosure callback);

  // Returns `"NEXUSENC1:" + base64(ciphertext)`. Returns `plaintext`
  // unchanged if it is empty or the encryptor isn't ready yet (degrades to
  // plaintext rather than dropping the secret — rare, writes happen late).
  std::string Encrypt(const std::string& plaintext);

  // Inverse of `Encrypt`. A marked value is decrypted (empty string if the
  // encryptor isn't ready or decryption fails). An UNMARKED value is
  // returned as-is (legacy plaintext / already-decrypted).
  //
  // The decrypted result is MEMOIZED keyed by the ciphertext, so the OS
  // crypto runs at most once per distinct value per process — important
  // because hot secrets (the model api_key / OAuth token) are read many
  // times and the OS decrypt is comparatively expensive.
  std::string Decrypt(const std::string& stored);

  // True iff `stored` carries the encryption marker.
  static bool IsEncrypted(const std::string& stored);

  void SetEncryptorForTesting(os_crypt_async::Encryptor encryptor);
  // Puts the singleton back into the not-yet-acquired state so a test can
  // exercise the "encryptor not ready" paths (Encrypt passthrough, Decrypt
  // → "", the byte-compare fallback in StoredApiKeysDiffer).
  void ClearEncryptorForTesting();
  // Number of real OS decrypt operations performed (cache misses). Lets a
  // test assert that N reads of the same value cost exactly 1 crypto op.
  size_t decrypt_ops_for_testing() const { return decrypt_ops_; }

 private:
  friend class base::NoDestructor<NexusSecretCrypt>;
  NexusSecretCrypt();
  ~NexusSecretCrypt();

  void OnEncryptor(os_crypt_async::Encryptor encryptor);

  std::optional<os_crypt_async::Encryptor> encryptor_;
  bool acquiring_ = false;
  std::vector<base::OnceClosure> ready_callbacks_;

  // Memoized decrypt results, keyed by the stored ciphertext (the full
  // "NEXUSENC1:…" string). Bounded by the number of distinct secret values
  // (small). Plaintext lives in process memory either way — the providers
  // already cache the decrypted api_key — so this adds no new exposure.
  std::map<std::string, std::string> decrypt_cache_;
  size_t decrypt_ops_ = 0;

  base::WeakPtrFactory<NexusSecretCrypt> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_SECRET_CRYPT_H_
