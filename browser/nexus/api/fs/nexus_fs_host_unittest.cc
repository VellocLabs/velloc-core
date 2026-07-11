// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
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

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  // Drain the ctor's CreateDirectory PostTask so the cwd exists before
  // we call AppendFile (which itself runs synchronously on this thread).
  base::RunLoop().RunUntilIdle();

  auto append = [&](const std::string& chunk) {
    std::vector<uint8_t> bytes(chunk.begin(), chunk.end());
    NexusFsHost::SimpleResult result = host->AppendFile(
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

// Regression: a missing file must report ENOENT-style wording, not the
// generic "Failed to read file". The JS guest's isMissingFileError()
// detector keys off "ENOENT" / "no such file" so that an absent
// ~/.nexus/settings.json on first run is treated as the benign defaults
// case instead of logging a spurious read warning.
TEST(NexusFsHostTest, ReadFileMissingReportsEnoent) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadResult result = host->ReadFile("settings.json");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.data.has_value());
  // Must contain a marker the guest's regex recognizes.
  EXPECT_NE(std::string::npos, result.error.find("ENOENT")) << result.error;
}

// Happy path stays intact: an existing file reads back its exact bytes.
TEST(NexusFsHostTest, ReadFileReturnsContentsForExistingFile) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  const std::string payload = "{\"version\":1}\n";
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("settings.json"),
                              payload));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadResult result = host->ReadFile("settings.json");
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_TRUE(result.data.has_value());
  const std::string read(reinterpret_cast<const char*>(result.data->data()),
                         result.data->size());
  EXPECT_EQ(payload, read);
}

}  // namespace
}  // namespace nexus
