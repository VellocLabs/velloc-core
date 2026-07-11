// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"

#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusMcpServerStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusMcpServerConfigs);
  }

  void SetServers(base::DictValue servers) {
    prefs_.SetDict(prefs::kNexusMcpServerConfigs, std::move(servers));
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(NexusMcpServerStoreTest, EmptyWhenNoServers) {
  NexusMcpServerStore store(&prefs_);
  EXPECT_TRUE(store.ListSummaries().empty());
  EXPECT_FALSE(store.GetConfig("ghost").has_value());
}

TEST_F(NexusMcpServerStoreTest, NullPrefsIsSafe) {
  NexusMcpServerStore store(nullptr);
  EXPECT_TRUE(store.ListSummaries().empty());
  EXPECT_FALSE(store.GetConfig("x").has_value());
}

TEST_F(NexusMcpServerStoreTest, ParsesStdioServerWithSecretEnv) {
  base::DictValue github;
  github.Set("transport", "stdio");
  github.Set("command", "node");
  base::ListValue args;
  args.Append("/srv/github.js");
  args.Append("--verbose");
  github.Set("args", std::move(args));
  base::DictValue env;
  env.Set("GITHUB_TOKEN", "ghp_secret_xyz");  // secret stays native
  github.Set("env", std::move(env));
  github.Set("cwd", "/work");
  base::DictValue servers;
  servers.Set("github", std::move(github));
  SetServers(std::move(servers));

  NexusMcpServerStore store(&prefs_);

  auto summaries = store.ListSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  EXPECT_EQ(summaries[0].id, "github");
  EXPECT_EQ(summaries[0].transport, "stdio");

  auto config = store.GetConfig("github");
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->transport, "stdio");
  EXPECT_EQ(config->command, "node");
  ASSERT_EQ(config->args.size(), 2u);
  EXPECT_EQ(config->args[0], "/srv/github.js");
  EXPECT_EQ(config->args[1], "--verbose");
  ASSERT_EQ(config->env.size(), 1u);
  EXPECT_EQ(config->env[0].first, "GITHUB_TOKEN");
  EXPECT_EQ(config->env[0].second, "ghp_secret_xyz");
  EXPECT_EQ(config->cwd.AsUTF8Unsafe(), "/work");
}

TEST_F(NexusMcpServerStoreTest, ParsesHttpServerWithAuthHeader) {
  base::DictValue linear;
  linear.Set("transport", "http");
  linear.Set("url", "https://mcp.linear.app/mcp");
  base::DictValue headers;
  headers.Set("Authorization", "Bearer secret_token");  // secret stays native
  linear.Set("headers", std::move(headers));
  base::DictValue servers;
  servers.Set("linear", std::move(linear));
  SetServers(std::move(servers));

  NexusMcpServerStore store(&prefs_);

  auto summaries = store.ListSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  EXPECT_EQ(summaries[0].transport, "http");

  auto config = store.GetConfig("linear");
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->transport, "http");
  EXPECT_EQ(config->url, "https://mcp.linear.app/mcp");
  ASSERT_EQ(config->headers.size(), 1u);
  EXPECT_EQ(config->headers[0].first, "Authorization");
  EXPECT_EQ(config->headers[0].second, "Bearer secret_token");
}

TEST_F(NexusMcpServerStoreTest, DefaultsTransportToStdio) {
  base::DictValue srv;
  srv.Set("command", "uvx");  // no explicit transport
  base::DictValue servers;
  servers.Set("local", std::move(srv));
  SetServers(std::move(servers));

  NexusMcpServerStore store(&prefs_);
  ASSERT_EQ(store.ListSummaries().size(), 1u);
  EXPECT_EQ(store.ListSummaries()[0].transport, "stdio");
  auto config = store.GetConfig("local");
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->transport, "stdio");
  EXPECT_EQ(config->command, "uvx");
}

TEST_F(NexusMcpServerStoreTest, RejectsStdioWithoutCommand) {
  base::DictValue srv;
  srv.Set("transport", "stdio");  // no command
  base::DictValue servers;
  servers.Set("bad", std::move(srv));
  SetServers(std::move(servers));

  NexusMcpServerStore store(&prefs_);
  // Still summarized (id+transport), but GetConfig fails (unusable).
  EXPECT_EQ(store.ListSummaries().size(), 1u);
  EXPECT_FALSE(store.GetConfig("bad").has_value());
}

TEST_F(NexusMcpServerStoreTest, RejectsHttpWithoutUrl) {
  base::DictValue srv;
  srv.Set("transport", "http");  // no url
  base::DictValue servers;
  servers.Set("bad", std::move(srv));
  SetServers(std::move(servers));

  NexusMcpServerStore store(&prefs_);
  EXPECT_FALSE(store.GetConfig("bad").has_value());
}

TEST_F(NexusMcpServerStoreTest, ListsMultipleServers) {
  base::DictValue stdio_srv;
  stdio_srv.Set("transport", "stdio");
  stdio_srv.Set("command", "node");
  base::DictValue http_srv;
  http_srv.Set("transport", "http");
  http_srv.Set("url", "https://example.test/mcp");
  base::DictValue servers;
  servers.Set("a", std::move(stdio_srv));
  servers.Set("b", std::move(http_srv));
  SetServers(std::move(servers));

  NexusMcpServerStore store(&prefs_);
  EXPECT_EQ(store.ListSummaries().size(), 2u);
}

}  // namespace
}  // namespace nexus
