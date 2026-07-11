// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/provider_config.h"

#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class ProviderConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusProviderConfigs);
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(ProviderConfigTest, GetStoredProviderConfigReturnsNullForNullPrefs) {
  EXPECT_EQ(GetStoredProviderConfig(nullptr, "openai"), nullptr);
}

TEST_F(ProviderConfigTest, GetStoredProviderConfigReturnsNullForMissingEntry) {
  EXPECT_EQ(GetStoredProviderConfig(&prefs_, "openai"), nullptr);
}

TEST_F(ProviderConfigTest, GetStoredProviderConfigReturnsEntryWhenPresent) {
  base::DictValue all_configs;
  base::DictValue openai_config;
  openai_config.Set("api_key", "sk-live-xxx");
  openai_config.Set("default_model", "gpt-4o");
  all_configs.Set("openai", std::move(openai_config));
  prefs_.SetDict(prefs::kNexusProviderConfigs, std::move(all_configs));

  const base::DictValue* config = GetStoredProviderConfig(&prefs_, "openai");
  ASSERT_NE(config, nullptr);
  const std::string* api_key = config->FindString("api_key");
  ASSERT_NE(api_key, nullptr);
  EXPECT_EQ(*api_key, "sk-live-xxx");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdPrefersDefaultModelField) {
  base::DictValue config;
  config.Set("default_model", "gpt-4o");
  config.Set("model", "gpt-3.5-turbo");
  EXPECT_EQ(GetStoredDefaultModelId(config), "gpt-4o");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdFallsBackToLegacyModelField) {
  base::DictValue config;
  config.Set("model", "gpt-3.5-turbo");
  EXPECT_EQ(GetStoredDefaultModelId(config), "gpt-3.5-turbo");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdIgnoresEmptyValues) {
  base::DictValue config;
  config.Set("default_model", "");
  config.Set("model", "gpt-3.5-turbo");
  EXPECT_EQ(GetStoredDefaultModelId(config), "gpt-3.5-turbo");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdReturnsEmptyWhenNeitherPresent) {
  base::DictValue config;
  config.Set("api_key", "sk-live-xxx");
  EXPECT_TRUE(GetStoredDefaultModelId(config).empty());
}

}  // namespace
}  // namespace nexus
