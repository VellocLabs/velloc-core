// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/secrets/nexus_secrets_bridge_adapter.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/values.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  base::Value last_success;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

class NexusSecretsBridgeAdapterTest : public testing::Test {
 protected:
  NexusSecretsBridgeAdapterTest() {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusSecrets);
    NexusSecretCrypt::GetInstance()->SetEncryptorForTesting(
        os_crypt_async::GetTestEncryptorForTesting());
  }

  // Dispatches and returns the success payload (empty dict on an error).
  base::DictValue Dispatch(std::string_view action, base::DictValue payload) {
    base::DictValue route;
    route.Set("namespace", "nexus");
    route.Set("capability", "secrets");
    route.Set("action", std::string(action));
    delegate_.last_success = base::Value();
    EXPECT_TRUE(adapter_.Dispatch(action, "req", route, payload, {}));
    task_environment_.RunUntilIdle();
    return delegate_.last_success.is_dict()
               ? delegate_.last_success.GetDict().Clone()
               : base::DictValue();
  }

  content::BrowserTaskEnvironment task_environment_;
  TestingPrefServiceSimple prefs_;
  RecordingDelegate delegate_;
  NexusSecretsBridgeAdapter adapter_{&delegate_, &prefs_};
};

TEST_F(NexusSecretsBridgeAdapterTest, SetHasListDeleteAndNoGet) {
  base::DictValue set;
  set.Set("name", "mcp:github");
  set.Set("value", "ghp_secret");
  set.Set("origin", "https://api.github.com/");
  Dispatch("set", std::move(set));
  EXPECT_EQ(delegate_.success_count, 1);

  base::DictValue has;
  has.Set("name", "mcp:github");
  const base::DictValue has_result = Dispatch("has", std::move(has));
  EXPECT_EQ(has_result.FindBool("exists"), true);
  ASSERT_TRUE(has_result.FindString("origin"));
  EXPECT_EQ(*has_result.FindString("origin"), "https://api.github.com");
  // The value never comes back.
  EXPECT_FALSE(has_result.Find("value"));

  base::DictValue list;
  list.Set("namespace", "mcp");
  const base::DictValue listed = Dispatch("listNames", std::move(list));
  const base::ListValue* secrets = listed.FindList("secrets");
  ASSERT_TRUE(secrets);
  ASSERT_EQ(secrets->size(), 1u);
  const base::DictValue& row = (*secrets)[0].GetDict();
  EXPECT_EQ(*row.FindString("name"), "mcp:github");
  EXPECT_FALSE(row.Find("value"));

  base::DictValue other_ns;
  other_ns.Set("namespace", "plugin");
  const base::DictValue other = Dispatch("listNames", std::move(other_ns));
  ASSERT_TRUE(other.FindList("secrets"));
  EXPECT_TRUE(other.FindList("secrets")->empty());

  // There is no `get` action at all.
  base::DictValue get;
  get.Set("name", "mcp:github");
  EXPECT_FALSE(adapter_.Dispatch("get", "req", base::DictValue(), get, {}));

  base::DictValue del;
  del.Set("name", "mcp:github");
  EXPECT_EQ(Dispatch("delete", std::move(del)).FindBool("deleted"), true);
}

TEST_F(NexusSecretsBridgeAdapterTest, InvalidNamesAndOriginsAreRefused) {
  base::DictValue set;
  set.Set("name", "github");
  set.Set("value", "x");
  set.Set("origin", "https://api.github.com");
  Dispatch("set", std::move(set));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue plain_http;
  plain_http.Set("name", "plugin:x");
  plain_http.Set("value", "x");
  plain_http.Set("origin", "http://api.example.com");
  Dispatch("set", std::move(plain_http));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  // An origin is required unless the secret is for process use.
  base::DictValue no_origin;
  no_origin.Set("name", "mcp:x");
  no_origin.Set("value", "x");
  Dispatch("set", std::move(no_origin));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_EQ(delegate_.success_count, 0);
}

TEST_F(NexusSecretsBridgeAdapterTest, ProcessEnvSecretsListTheirMark) {
  base::DictValue set;
  set.Set("name", "mcp:server:API_TOKEN");
  set.Set("value", "tok");
  set.Set("processEnv", true);
  Dispatch("set", std::move(set));
  EXPECT_EQ(delegate_.success_count, 1);

  const base::DictValue listed = Dispatch("listNames", base::DictValue());
  const base::ListValue* secrets = listed.FindList("secrets");
  ASSERT_TRUE(secrets);
  ASSERT_EQ(secrets->size(), 1u);
  const base::DictValue& row = (*secrets)[0].GetDict();
  EXPECT_EQ(row.FindBool("processEnv"), true);
  EXPECT_EQ(*row.FindString("origin"), "");
  EXPECT_FALSE(row.Find("value"));

  base::DictValue provider;
  provider.Set("name", "provider:openai");
  provider.Set("value", "k");
  provider.Set("origin", "https://api.openai.com");
  provider.Set("processEnv", true);
  Dispatch("set", std::move(provider));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

}  // namespace
}  // namespace nexus
