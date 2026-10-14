// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/secrets/nexus_secret_store.h"

#include <map>
#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace nexus {
namespace {

url::Origin OriginOf(const char* url) {
  return url::Origin::Create(GURL(url));
}

class NexusSecretStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusSecrets);
    NexusSecretCrypt::GetInstance()->SetEncryptorForTesting(
        os_crypt_async::GetTestEncryptorForTesting());
  }

  TestingPrefServiceSimple prefs_;
};

TEST(NexusSecretNamesTest, NamesAreNamespaced) {
  EXPECT_TRUE(IsValidNamespacedSecretName("provider:openai"));
  EXPECT_TRUE(IsValidNamespacedSecretName("plugin:acme/token@v2"));
  EXPECT_TRUE(IsValidNamespacedSecretName("mcp:github:pat"));
  EXPECT_FALSE(IsValidNamespacedSecretName("openai"));
  EXPECT_FALSE(IsValidNamespacedSecretName("user:openai"));
  EXPECT_FALSE(IsValidNamespacedSecretName("provider:"));
  EXPECT_FALSE(IsValidNamespacedSecretName("provider:has space"));
  EXPECT_FALSE(IsValidNamespacedSecretName("provider:" + std::string(129, 'a')));
  EXPECT_EQ(SecretNamespace("mcp:github:pat"), "mcp");
}

TEST(NexusSecretNamesTest, OriginsAreHttpsOrLocalHttp) {
  EXPECT_EQ(CanonicalSecretOrigin("https://API.example.com/v1/x?y#z"),
            "https://api.example.com");
  EXPECT_EQ(CanonicalSecretOrigin("http://localhost:8080/"),
            "http://localhost:8080");
  EXPECT_EQ(CanonicalSecretOrigin("http://127.0.0.1:3000"),
            "http://127.0.0.1:3000");
  EXPECT_FALSE(CanonicalSecretOrigin("http://api.example.com"));
  EXPECT_FALSE(CanonicalSecretOrigin("https://user:pw@api.example.com"));
  EXPECT_FALSE(CanonicalSecretOrigin("not a url"));
  EXPECT_FALSE(CanonicalSecretOrigin("file:///etc/passwd"));
}

TEST(NexusSecretNamesTest, PlaceholdersExpandAndErrorsAreTyped) {
  std::map<std::string, SecretLookupResult> table;
  table["provider:a"] = {SecretLookupResult::Status::kFound, "AAA"};
  table["provider:b"] = {SecretLookupResult::Status::kOriginMismatch, ""};
  const NamespacedSecretLookup lookup = base::BindRepeating(
      [](const std::map<std::string, SecretLookupResult>* table,
         const std::string& name) {
        auto it = table->find(name);
        return it == table->end() ? SecretLookupResult() : it->second;
      },
      &table);

  std::string out;
  EXPECT_FALSE(ExpandSecretPlaceholders("Bearer {{secret:provider:a}}!",
                                        lookup, &out));
  EXPECT_EQ(out, "Bearer AAA!");
  EXPECT_FALSE(ExpandSecretPlaceholders("no placeholder", lookup, &out));
  EXPECT_EQ(out, "no placeholder");

  EXPECT_EQ(ExpandSecretPlaceholders("{{secret:provider:b}}", lookup, &out)
                ->code,
            "SECRET_ORIGIN_MISMATCH");
  EXPECT_EQ(ExpandSecretPlaceholders("{{secret:provider:zzz}}", lookup, &out)
                ->code,
            "SECRET_UNAVAILABLE");
  EXPECT_EQ(ExpandSecretPlaceholders("{{secret:provider:a", lookup, &out)
                ->code,
            "INVALID_ARGS");
  EXPECT_EQ(ExpandSecretPlaceholders("{{secret:bad}}", lookup, &out)->code,
            "INVALID_ARGS");
  EXPECT_TRUE(HasSecretPlaceholder("x {{secret:provider:a}}"));
  EXPECT_FALSE(HasSecretPlaceholder("{{other}}"));
}

TEST_F(NexusSecretStoreTest, SetIsSealedAndOnlyItsOriginCanRead) {
  NexusSecretStore store(&prefs_);
  ASSERT_FALSE(store.Set("plugin:acme", "s3cret", "https://api.acme.dev/v1"));

  // Sealed at rest: the plaintext is nowhere in the pref.
  const base::DictValue* entry =
      prefs_.GetDict(prefs::kNexusSecrets).FindDict("plugin:acme");
  ASSERT_TRUE(entry);
  ASSERT_TRUE(entry->FindString("value"));
  EXPECT_TRUE(NexusSecretCrypt::IsEncrypted(*entry->FindString("value")));
  EXPECT_EQ(entry->FindString("value")->find("s3cret"), std::string::npos);
  EXPECT_EQ(*entry->FindString("origin"), "https://api.acme.dev");

  const SecretLookupResult same =
      store.Lookup("plugin:acme", OriginOf("https://api.acme.dev/other"));
  EXPECT_EQ(same.status, SecretLookupResult::Status::kFound);
  EXPECT_EQ(same.value, "s3cret");
  EXPECT_EQ(store.Lookup("plugin:acme", OriginOf("https://evil.example"))
                .status,
            SecretLookupResult::Status::kOriginMismatch);
  // A different port is a different origin.
  EXPECT_EQ(store.Lookup("plugin:acme", OriginOf("https://api.acme.dev:8443"))
                .status,
            SecretLookupResult::Status::kOriginMismatch);
  EXPECT_EQ(store.Lookup("plugin:none", OriginOf("https://api.acme.dev"))
                .status,
            SecretLookupResult::Status::kMissing);
}

TEST_F(NexusSecretStoreTest, HasListDeleteNeverExposeValues) {
  NexusSecretStore store(&prefs_);
  ASSERT_FALSE(store.Set("provider:openai", "k1", "https://api.openai.com"));
  ASSERT_FALSE(store.Set("mcp:github", "k2", "https://api.github.com"));

  EXPECT_EQ(store.Has("provider:openai"), "https://api.openai.com");
  EXPECT_FALSE(store.Has("provider:other"));

  const auto all = store.ListNames(std::nullopt);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].name, "mcp:github");
  EXPECT_EQ(all[1].name, "provider:openai");
  const auto mcp = store.ListNames(std::string("mcp"));
  ASSERT_EQ(mcp.size(), 1u);
  EXPECT_EQ(mcp[0].origin, "https://api.github.com");

  EXPECT_TRUE(store.Delete("mcp:github"));
  EXPECT_FALSE(store.Delete("mcp:github"));
  EXPECT_EQ(store.ListNames(std::nullopt).size(), 1u);
}

TEST_F(NexusSecretStoreTest, RefusesBadInputAndAnUnreadyKeystore) {
  NexusSecretStore store(&prefs_);
  EXPECT_EQ(store.Set("nonamespace", "v", "https://a.example")->code,
            "INVALID_ARGS");
  EXPECT_EQ(store.Set("plugin:x", "", "https://a.example")->code,
            "INVALID_ARGS");
  EXPECT_EQ(store.Set("plugin:x", "v", "http://a.example")->code,
            "INVALID_ARGS");

  NexusSecretCrypt::GetInstance()->ClearEncryptorForTesting();
  EXPECT_EQ(store.Set("plugin:x", "v", "https://a.example")->code,
            "SECRET_STORE_UNAVAILABLE");
  // Nothing — certainly not plaintext — was stored.
  EXPECT_FALSE(store.Has("plugin:x"));
}

TEST_F(NexusSecretStoreTest, ProcessEnvIsOptInAndOnlyForPluginAndMcp) {
  NexusSecretStore store(&prefs_);
  // Provider keys never go into a child's env.
  EXPECT_EQ(store.Set("provider:openai", "k", "https://api.openai.com",
                      /*process_env=*/true)
                ->code,
            "INVALID_ARGS");
  // No origin is only accepted for a process-only secret.
  EXPECT_EQ(store.Set("mcp:token", "v", "")->code, "INVALID_ARGS");

  ASSERT_FALSE(store.Set("mcp:token", "env-secret", "", /*process_env=*/true));
  ASSERT_FALSE(store.Set("plugin:both", "both-secret", "https://a.example",
                         /*process_env=*/true));
  ASSERT_FALSE(store.Set("plugin:net", "net-secret", "https://a.example"));

  EXPECT_EQ(store.LookupForProcessEnv("mcp:token").value, "env-secret");
  EXPECT_EQ(store.LookupForProcessEnv("plugin:both").value, "both-secret");
  EXPECT_EQ(store.LookupForProcessEnv("plugin:net").status,
            SecretLookupResult::Status::kNotPermitted);
  EXPECT_EQ(store.LookupForProcessEnv("plugin:none").status,
            SecretLookupResult::Status::kMissing);

  // A process-only secret reaches no network request; a marked one still
  // reaches its own origin.
  EXPECT_EQ(store.Lookup("mcp:token", OriginOf("https://a.example")).status,
            SecretLookupResult::Status::kOriginMismatch);
  EXPECT_EQ(store.Lookup("plugin:both", OriginOf("https://a.example")).value,
            "both-secret");

  std::string out;
  EXPECT_FALSE(ExpandSecretPlaceholders("Bearer {{secret:mcp:token}}",
                                        store.ProcessEnvLookup(), &out));
  EXPECT_EQ(out, "Bearer env-secret");
  EXPECT_EQ(ExpandSecretPlaceholders("{{secret:plugin:net}}",
                                     store.ProcessEnvLookup(), &out)
                ->code,
            "SECRET_NOT_PERMITTED");

  const auto listed = store.ListNames(std::nullopt);
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0].name, "mcp:token");
  EXPECT_TRUE(listed[0].process_env);
  EXPECT_EQ(listed[0].origin, "");
  EXPECT_EQ(listed[2].name, "plugin:net");
  EXPECT_FALSE(listed[2].process_env);

  // Replacing a secret without the mark clears it.
  ASSERT_FALSE(store.Set("plugin:both", "both-2", "https://a.example"));
  EXPECT_EQ(store.LookupForProcessEnv("plugin:both").status,
            SecretLookupResult::Status::kNotPermitted);
}

}  // namespace
}  // namespace nexus
