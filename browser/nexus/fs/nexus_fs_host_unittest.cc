// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/fs/nexus_fs_host.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(NexusFsHostTest, AppendFileCreatesMissingFileAndAppendsData) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  NexusFsHost host(temp_dir.GetPath());

  auto append = [&](const std::string& chunk) {
    base::RunLoop run_loop;
    bool success = false;
    std::optional<std::string> error_message;
    std::vector<uint8_t> bytes(chunk.begin(), chunk.end());

    host.AppendFile(
        "session.jsonl", mojo_base::BigBuffer(base::span(bytes)), 0666,
        base::BindOnce(
            [&](bool append_success,
                std::optional<std::string> append_error_message) {
              success = append_success;
              error_message = std::move(append_error_message);
              run_loop.Quit();
            }));

    run_loop.Run();
    EXPECT_TRUE(success) << error_message.value_or("AppendFile failed");
  };

  append("{\"first\":true}\n");
  append("{\"second\":true}\n");

  std::string content;
  ASSERT_TRUE(base::ReadFileToString(
      temp_dir.GetPath().AppendASCII("session.jsonl"), &content));
  EXPECT_EQ("{\"first\":true}\n{\"second\":true}\n", content);
}

}  // namespace
}  // namespace nexus
