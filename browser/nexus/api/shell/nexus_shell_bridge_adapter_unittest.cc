// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/nexus_shell_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/environment.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int error_count = 0;
  base::Value last_success;
  std::string last_error_code;
  std::string last_error_message;

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
                          std::string_view /*result_binary_field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& message) override {
    ++error_count;
    last_error_code = code;
    last_error_message = message;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue ShellRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "shell");
  route.Set("action", std::string(action));
  return route;
}

class NexusShellBridgeAdapterTest : public testing::Test {
 protected:
  // No profile: everything below is answered before the launcher is needed.
  void Dispatch(std::string_view action, base::DictValue payload) {
    EXPECT_TRUE(adapter_.Dispatch(action, "req", ShellRoute(action), payload,
                                  {}));
    task_environment_.RunUntilIdle();
  }

  content::BrowserTaskEnvironment task_environment_;
  RecordingDelegate delegate_;
  NexusShellBridgeAdapter adapter_{&delegate_, nullptr};
};

TEST_F(NexusShellBridgeAdapterTest, SpawnTakesExactlyOneForm) {
  base::DictValue both;
  both.Set("shell", "bash");
  both.Set("command", "echo hi");
  both.Set("file", "node");
  Dispatch("spawn", std::move(both));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  Dispatch("spawn", base::DictValue());
  EXPECT_EQ(delegate_.error_count, 2);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue empty_file;
  empty_file.Set("file", "");
  Dispatch("spawn", std::move(empty_file));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

TEST_F(NexusShellBridgeAdapterTest, PersistNeedsAnAbsoluteLogPath) {
  base::DictValue payload;
  payload.Set("file", "node");
  base::ListValue args;
  args.Append("server.js");
  payload.Set("args", std::move(args));
  base::DictValue persist;
  persist.Set("logPath", "relative/out.log");
  payload.Set("persist", std::move(persist));
  Dispatch("spawn", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_NE(delegate_.last_error_message.find("logPath"), std::string::npos);
}

// A well-formed argv spawn with no profile reaches the launch step, which is
// where SHELL_UNAVAILABLE is decided — i.e. the form was accepted.
TEST_F(NexusShellBridgeAdapterTest, AWellFormedArgvSpawnPassesValidation) {
  base::DictValue payload;
  payload.Set("file", "node");
  payload.Set("args", base::ListValue());
  Dispatch("spawn", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "SHELL_UNAVAILABLE");
}

// `{{secret:NAME}}` in an env value is resolved before the launch, and a
// secret that cannot be resolved refuses the spawn instead of passing the
// placeholder text to the child. (No profile here, so no secret resolves; the
// resolving and the processEnv rule are NexusSecretStoreTest's.)
TEST_F(NexusShellBridgeAdapterTest, EnvSecretPlaceholdersResolveOrRefuse) {
  auto spawn_with_env = [&](const char* value) {
    base::DictValue payload;
    payload.Set("file", "node");
    payload.Set("args", base::ListValue());
    base::DictValue env;
    env.Set("API_TOKEN", value);
    payload.Set("env", std::move(env));
    Dispatch("spawn", std::move(payload));
  };
  spawn_with_env("{{secret:mcp:server:API_TOKEN}}");
  EXPECT_EQ(delegate_.last_error_code, "SECRET_UNAVAILABLE");
  spawn_with_env("{{secret:mcp:unterminated");
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  // A plain value is not a placeholder: the spawn reaches the launch step.
  spawn_with_env("plain");
  EXPECT_EQ(delegate_.last_error_code, "SHELL_UNAVAILABLE");
}

TEST_F(NexusShellBridgeAdapterTest, EnvReadsSetAndUnsetVariables) {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  ASSERT_TRUE(env->SetVar("NEXUS_SHELL_ENV_TEST", "value-1"));
  env->UnSetVar("NEXUS_SHELL_ENV_TEST_UNSET");

  base::DictValue payload;
  base::ListValue names;
  names.Append("NEXUS_SHELL_ENV_TEST");
  names.Append("NEXUS_SHELL_ENV_TEST_UNSET");
  payload.Set("names", std::move(names));
  Dispatch("env", std::move(payload));

  ASSERT_EQ(delegate_.success_count, 1);
  const base::DictValue* values =
      delegate_.last_success.GetDict().FindDict("values");
  ASSERT_TRUE(values);
  EXPECT_EQ(*values->FindString("NEXUS_SHELL_ENV_TEST"), "value-1");
  const base::Value* unset = values->Find("NEXUS_SHELL_ENV_TEST_UNSET");
  ASSERT_TRUE(unset);
  EXPECT_TRUE(unset->is_none());
  env->UnSetVar("NEXUS_SHELL_ENV_TEST");
}

TEST_F(NexusShellBridgeAdapterTest, EnvRefusesTooManyNames) {
  base::DictValue payload;
  base::ListValue names;
  for (size_t i = 0; i <= NexusShellBridgeAdapter::kMaxEnvNames; ++i) {
    names.Append("N");
  }
  payload.Set("names", std::move(names));
  Dispatch("env", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

}  // namespace
}  // namespace nexus
