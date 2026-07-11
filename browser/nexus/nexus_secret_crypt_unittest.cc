// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

#include <string>

#include "components/os_crypt/async/browser/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

NexusSecretCrypt* CryptWithTestKey() {
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  crypt->SetEncryptorForTesting(os_crypt_async::GetTestEncryptorForTesting());
  return crypt;
}

TEST(NexusSecretCryptTest, EncryptThenDecryptRoundTrips) {
  NexusSecretCrypt* crypt = CryptWithTestKey();
  const std::string secret = "sk-ant-super-secret-token";

  const std::string encrypted = crypt->Encrypt(secret);
  EXPECT_NE(encrypted, secret);
  EXPECT_TRUE(NexusSecretCrypt::IsEncrypted(encrypted));
  // The plaintext must not be recoverable by eyeballing the stored value.
  EXPECT_EQ(encrypted.find(secret), std::string::npos);

  EXPECT_EQ(crypt->Decrypt(encrypted), secret);
}

TEST(NexusSecretCryptTest, DecryptPassesThroughLegacyPlaintext) {
  NexusSecretCrypt* crypt = CryptWithTestKey();
  // A value WITHOUT the marker is legacy plaintext (or already-decrypted)
  // and must round-trip unchanged — this is what keeps existing
  // plaintext-seeded api_keys / MCP env working after the migration.
  EXPECT_FALSE(NexusSecretCrypt::IsEncrypted("plain-legacy-api-key"));
  EXPECT_EQ(crypt->Decrypt("plain-legacy-api-key"), "plain-legacy-api-key");
}

TEST(NexusSecretCryptTest, EmptyStaysEmpty) {
  NexusSecretCrypt* crypt = CryptWithTestKey();
  EXPECT_EQ(crypt->Encrypt(std::string()), std::string());
  EXPECT_FALSE(NexusSecretCrypt::IsEncrypted(std::string()));
}

TEST(NexusSecretCryptTest, DistinctSecretsProduceDistinctCiphertext) {
  NexusSecretCrypt* crypt = CryptWithTestKey();
  EXPECT_NE(crypt->Encrypt("token-A"), crypt->Encrypt("token-B"));
}

// Encrypt is idempotent: re-encrypting an already-encrypted value (the
// edit-form round-trip case) must NOT add a second layer, and must still
// decrypt to the original in one step.
TEST(NexusSecretCryptTest, EncryptIsIdempotent) {
  NexusSecretCrypt* crypt = CryptWithTestKey();
  const std::string secret = "sk-edit-roundtrip";

  const std::string once = crypt->Encrypt(secret);
  const std::string twice = crypt->Encrypt(once);  // re-encrypt ciphertext
  EXPECT_EQ(twice, once);                           // no second layer added
  EXPECT_EQ(crypt->Decrypt(twice), secret);         // one-step decrypt
}

// A hot secret (the model api_key / token) is read many times, but the OS
// decrypt must run only once per distinct ciphertext — the rest are served
// from the memoization cache.
TEST(NexusSecretCryptTest, DecryptRunsCryptoOncePerValueAcrossManyReads) {
  NexusSecretCrypt* crypt = CryptWithTestKey();  // resets the op counter
  const std::string enc = crypt->Encrypt("hot-model-api-key");

  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(crypt->Decrypt(enc), "hot-model-api-key");
  }
  // 50 reads, exactly 1 real OS decrypt.
  EXPECT_EQ(crypt->decrypt_ops_for_testing(), 1u);
}

}  // namespace
}  // namespace nexus
