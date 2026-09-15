// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"

namespace nexus {

namespace {

// Self-describing prefix for an encrypted secret. A real api_key / token /
// header value never begins with this, so its presence unambiguously marks
// a value as ciphertext (and its absence marks legacy plaintext).
constexpr std::string_view kMarker = "NEXUSENC1:";

}  // namespace

// static
NexusSecretCrypt* NexusSecretCrypt::GetInstance() {
  static base::NoDestructor<NexusSecretCrypt> instance;
  return instance.get();
}

NexusSecretCrypt::NexusSecretCrypt() = default;
NexusSecretCrypt::~NexusSecretCrypt() = default;

void NexusSecretCrypt::EnsureAcquired(os_crypt_async::OSCryptAsync* service) {
  if (encryptor_.has_value() || acquiring_ || !service) {
    return;
  }
  acquiring_ = true;
  service->GetInstance(base::BindOnce(&NexusSecretCrypt::OnEncryptor,
                                      weak_factory_.GetWeakPtr()));
}

void NexusSecretCrypt::OnEncryptor(os_crypt_async::Encryptor encryptor) {
  encryptor_ = std::move(encryptor);
  acquiring_ = false;
  std::vector<base::OnceClosure> callbacks;
  callbacks.swap(ready_callbacks_);
  for (auto& callback : callbacks) {
    std::move(callback).Run();
  }
}

void NexusSecretCrypt::WhenReady(base::OnceClosure callback) {
  if (encryptor_.has_value()) {
    std::move(callback).Run();
    return;
  }
  ready_callbacks_.push_back(std::move(callback));
}

std::string NexusSecretCrypt::Encrypt(const std::string& plaintext) {
  if (plaintext.empty() || !encryptor_.has_value()) {
    return plaintext;
  }
  // Idempotent: never double-encrypt. A value that already carries the
  // marker (e.g. an edit-form round-trip that re-submits the stored
  // ciphertext) is returned unchanged, so the same secret is encrypted
  // exactly once and Decrypt peels exactly one layer.
  if (IsEncrypted(plaintext)) {
    return plaintext;
  }
  std::string ciphertext;
  if (!encryptor_->EncryptString(plaintext, &ciphertext)) {
    return plaintext;
  }
  return std::string(kMarker) + base::Base64Encode(ciphertext);
}

std::string NexusSecretCrypt::Decrypt(const std::string& stored) {
  if (!IsEncrypted(stored)) {
    return stored;  // legacy plaintext / already-decrypted
  }
  if (!encryptor_.has_value()) {
    return std::string();  // marked but encryptor not ready
  }
  // Memoize: a hot secret (api_key / token) is read many times; the OS
  // decrypt should run only on the first read of each distinct ciphertext.
  if (auto it = decrypt_cache_.find(stored); it != decrypt_cache_.end()) {
    return it->second;
  }
  std::string ciphertext;
  if (!base::Base64Decode(std::string_view(stored).substr(kMarker.size()),
                          &ciphertext)) {
    return std::string();
  }
  std::string plaintext;
  if (!encryptor_->DecryptString(ciphertext, &plaintext)) {
    // The value carries our marker but this OS keystore cannot open it —
    // typically a sealed settings.json / pref copied from another machine or
    // OS user, or a reset DPAPI/Keychain. Say so once per distinct value:
    // callers see an empty secret and would otherwise fail with a bare 401.
    LOG(ERROR) << "NexusSecretCrypt: a sealed secret could not be decrypted "
                  "by this OS keystore (copied from another machine or user?); "
                  "the secret must be re-entered";
    return std::string();
  }
  ++decrypt_ops_;
  decrypt_cache_[stored] = plaintext;
  return plaintext;
}

// static
bool NexusSecretCrypt::IsEncrypted(const std::string& stored) {
  return base::StartsWith(stored, kMarker);
}

void NexusSecretCrypt::SetEncryptorForTesting(
    os_crypt_async::Encryptor encryptor) {
  encryptor_ = std::move(encryptor);
  acquiring_ = false;
  decrypt_cache_.clear();
  decrypt_ops_ = 0;
}

void NexusSecretCrypt::ClearEncryptorForTesting() {
  encryptor_.reset();
  acquiring_ = false;
  decrypt_cache_.clear();
  decrypt_ops_ = 0;
}

}  // namespace nexus
