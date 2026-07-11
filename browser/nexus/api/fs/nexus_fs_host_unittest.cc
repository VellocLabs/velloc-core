// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

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

// NexusFsHost's per-method API is now synchronous (Phase 10e) — each
// returns a Result struct directly, with no callback / PostTask. The
// only async work is the ctor's one-shot CreateDirectory hop, which is
// why this test still uses TaskEnvironment + RunLoop.RunUntilIdle().
TEST(NexusFsHostTest, AppendFileCreatesMissingFileAndAppendsData) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  NexusFsHost host(temp_dir.GetPath());
  // Drain the ctor's CreateDirectory PostTask so the cwd exists before
  // we call AppendFile (which itself runs synchronously on this thread).
  base::RunLoop().RunUntilIdle();

  auto append = [&](const std::string& chunk) {
    std::vector<uint8_t> bytes(chunk.begin(), chunk.end());
    NexusFsHost::SimpleResult result = host.AppendFile(
        "session.jsonl", mojo_base::BigBuffer(base::span(bytes)));
    EXPECT_TRUE(result.success) << result.error;
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
