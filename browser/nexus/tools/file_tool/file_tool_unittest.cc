// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/file_tool/file_tool.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/tools/nexus_ripgrep_runner.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"

namespace nexus {
namespace {

constexpr char kSessionKey[] = "file-tool-test-session";

// Records how a ToolCallback settled so a test can assert on it once the
// async reply has run. Mirrors the (on_success, on_error) pair the real
// dispatcher binds onto every ToolCallback.
struct CallResult {
  bool resolved = false;
  bool rejected = false;
  base::Value value;
  std::string error_code;
  std::string error_message;
};

ToolCallback MakeRecordingCallback(CallResult* out) {
  auto on_success = base::BindLambdaForTesting([out](base::Value value) {
    out->resolved = true;
    out->value = std::move(value);
  });
  auto on_error =
      base::BindLambdaForTesting([out](std::string code, std::string message) {
        out->rejected = true;
        out->error_code = std::move(code);
        out->error_message = std::move(message);
      });
  return ToolCallback(std::move(on_success), std::move(on_error));
}

// Happy path: Read hops the blocking I/O to base::ThreadPool and resolves
// back on UI with the Claude-Code-style line-numbered projection.
TEST(FileToolTest, ReadResolvesWithLineNumberedProjection) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));

  // Drains both the ThreadPool worker (ReadFileBlocking) and the UI reply
  // (OnReadDone) that PostTaskAndReplyWithResult chains.
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_NE(std::string::npos, result.value.GetString().find("1\talpha"))
      << result.value.GetString();
}

TEST(FileToolTest, ReadDefaultLineLimitScalesWithContextWindow) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("long.txt");
  std::string contents;
  for (int i = 1; i <= 2100; ++i) {
    contents.append("line ");
    contents.append(std::to_string(i));
    contents.push_back('\n');
  }
  ASSERT_TRUE(base::WriteFile(file, contents));

  ToolSession small_session(/*profile=*/nullptr, kSessionKey);
  CallResult small_result;
  ::nexus::generated::ReadInput small_input;
  small_input.file_path = file.AsUTF8Unsafe();
  small_session.file_tool().RunReadOnUI(std::move(small_input),
                                        MakeRecordingCallback(&small_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(small_result.resolved);
  ASSERT_TRUE(small_result.value.is_string());
  const std::string& small = small_result.value.GetString();
  EXPECT_NE(std::string::npos, small.find("1000\tline 1000")) << small;
  EXPECT_EQ(std::string::npos, small.find("1001\tline 1001")) << small;
  EXPECT_NE(std::string::npos,
            small.find("[file truncated after line 1000"))
      << small;

  ToolSession large_session(/*profile=*/nullptr, kSessionKey);
  CallResult large_result;
  ::nexus::generated::ReadInput large_input;
  large_input.file_path = file.AsUTF8Unsafe();
  large_input.context_window = 500001;
  large_session.file_tool().RunReadOnUI(std::move(large_input),
                                        MakeRecordingCallback(&large_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(large_result.resolved);
  ASSERT_TRUE(large_result.value.is_string());
  const std::string& large = large_result.value.GetString();
  EXPECT_NE(std::string::npos, large.find("2000\tline 2000")) << large;
  EXPECT_EQ(std::string::npos, large.find("2001\tline 2001")) << large;
  EXPECT_NE(std::string::npos,
            large.find("[file truncated after line 2000"))
      << large;
}

TEST(FileToolTest, ReadDedupsUnchangedFullTextRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::ReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  CallResult second_result;
  ::nexus::generated::ReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_string());
  EXPECT_EQ(
      "File unchanged since last read. The content from the earlier Read "
      "tool_result in this conversation is still current — refer to that "
      "instead "
      "of re-reading.",
      second_result.value.GetString());
}

TEST(FileToolTest, ReadDoesNotDedupAfterContentChange) {
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::ReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  task_environment.FastForwardBy(base::Seconds(1));
  ASSERT_TRUE(base::WriteFile(file, "gamma\n"));

  CallResult second_result;
  ::nexus::generated::ReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_string());
  EXPECT_NE(std::string::npos, second_result.value.GetString().find("1\tgamma"))
      << second_result.value.GetString();
  EXPECT_EQ(std::string::npos, second_result.value.GetString().find(
                                   "File unchanged since last read"));
}

TEST(FileToolTest, RepeatedPartialReadDedupsExactRangeButKeepsEditWriteUnsafe) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::ReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  first_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);
  ASSERT_TRUE(first_result.value.is_string());
  EXPECT_NE(std::string::npos, first_result.value.GetString().find("1\talpha"))
      << first_result.value.GetString();

  CallResult second_result;
  ::nexus::generated::ReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  second_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_string());
  EXPECT_NE(std::string::npos, second_result.value.GetString().find(
                                   "File unchanged since last read"));

  CallResult edit_result;
  ::nexus::generated::EditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "beta";
  edit_input.new_string = "delta";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();
  EXPECT_FALSE(edit_result.resolved);
  EXPECT_TRUE(edit_result.rejected);
  EXPECT_EQ("file_not_read", edit_result.error_code);

  CallResult write_result;
  ::nexus::generated::WriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "delta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();
  EXPECT_FALSE(write_result.resolved);
  EXPECT_TRUE(write_result.rejected);
  EXPECT_EQ("file_not_read", write_result.error_code);
}

TEST(FileToolTest, PartialReadFollowedByFullReadDoesNotDedup) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::ReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  first_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  CallResult second_result;
  ::nexus::generated::ReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_string());
  EXPECT_NE(std::string::npos, second_result.value.GetString().find("1\talpha"))
      << second_result.value.GetString();
  EXPECT_NE(std::string::npos, second_result.value.GetString().find("2\tbeta"))
      << second_result.value.GetString();
  EXPECT_EQ(std::string::npos, second_result.value.GetString().find(
                                   "File unchanged since last read"));
}

TEST(FileToolTest, ExactRangeDedupMatrixRequiresSameOffsetAndLimit) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("matrix.txt");
  ASSERT_TRUE(base::WriteFile(file, "one\ntwo\nthree\nfour\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::ReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  first_input.offset = 2;
  first_input.limit = 2;
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);
  ASSERT_TRUE(first_result.value.is_string());
  EXPECT_NE(std::string::npos, first_result.value.GetString().find("2\ttwo"));
  EXPECT_NE(std::string::npos, first_result.value.GetString().find("3\tthree"));

  CallResult same_range_result;
  ::nexus::generated::ReadInput same_range_input;
  same_range_input.file_path = file.AsUTF8Unsafe();
  same_range_input.offset = 2;
  same_range_input.limit = 2;
  session.file_tool().RunReadOnUI(std::move(same_range_input),
                                  MakeRecordingCallback(&same_range_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(same_range_result.resolved);
  ASSERT_TRUE(same_range_result.value.is_string());
  EXPECT_NE(std::string::npos, same_range_result.value.GetString().find(
                                   "File unchanged since last read"));

  CallResult different_limit_result;
  ::nexus::generated::ReadInput different_limit_input;
  different_limit_input.file_path = file.AsUTF8Unsafe();
  different_limit_input.offset = 2;
  different_limit_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(different_limit_input),
                                  MakeRecordingCallback(&different_limit_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(different_limit_result.resolved);
  ASSERT_TRUE(different_limit_result.value.is_string());
  EXPECT_NE(std::string::npos,
            different_limit_result.value.GetString().find("2\ttwo"));
  EXPECT_EQ(std::string::npos, different_limit_result.value.GetString().find(
                                   "File unchanged since last read"));

  CallResult different_offset_result;
  ::nexus::generated::ReadInput different_offset_input;
  different_offset_input.file_path = file.AsUTF8Unsafe();
  different_offset_input.offset = 3;
  different_offset_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(different_offset_input),
                                  MakeRecordingCallback(&different_offset_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(different_offset_result.resolved);
  ASSERT_TRUE(different_offset_result.value.is_string());
  EXPECT_NE(std::string::npos,
            different_offset_result.value.GetString().find("3\tthree"));
  EXPECT_EQ(std::string::npos, different_offset_result.value.GetString().find(
                                   "File unchanged since last read"));
}

TEST(FileToolTest, EditAndWriteStatesCanDedupButDoNotRequirePriorRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::ReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  CallResult edit_result;
  ::nexus::generated::EditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "alpha";
  edit_input.new_string = "gamma";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(edit_result.resolved);

  CallResult post_edit_read_result;
  ::nexus::generated::ReadInput post_edit_read_input;
  post_edit_read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(post_edit_read_input),
                                  MakeRecordingCallback(&post_edit_read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(post_edit_read_result.resolved);
  ASSERT_TRUE(post_edit_read_result.value.is_string());
  EXPECT_NE(std::string::npos, post_edit_read_result.value.GetString().find(
                                   "1\tgamma"));
  EXPECT_EQ(std::string::npos, post_edit_read_result.value.GetString().find(
                                   "File unchanged since last read"));

  CallResult second_post_edit_read_result;
  ::nexus::generated::ReadInput second_post_edit_read_input;
  second_post_edit_read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_post_edit_read_input),
                                  MakeRecordingCallback(&second_post_edit_read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(second_post_edit_read_result.resolved);
  ASSERT_TRUE(second_post_edit_read_result.value.is_string());
  EXPECT_NE(std::string::npos,
            second_post_edit_read_result.value.GetString().find(
                "File unchanged since last read"));

  CallResult write_result;
  ::nexus::generated::WriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "delta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(write_result.resolved);

  CallResult post_write_read_result;
  ::nexus::generated::ReadInput post_write_read_input;
  post_write_read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(post_write_read_input),
                                  MakeRecordingCallback(&post_write_read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(post_write_read_result.resolved);
  ASSERT_TRUE(post_write_read_result.value.is_string());
  EXPECT_NE(std::string::npos,
            post_write_read_result.value.GetString().find("1\tdelta"));
  EXPECT_EQ(std::string::npos, post_write_read_result.value.GetString().find(
                                   "File unchanged since last read"));
}
TEST(FileToolTest, ClearReadStateDisablesDedup) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::ReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  session.file_tool().ClearReadState();

  CallResult second_result;
  ::nexus::generated::ReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_string());
  EXPECT_NE(std::string::npos, second_result.value.GetString().find("1\talpha"))
      << second_result.value.GetString();
}

TEST(FileToolTest, ReadIgnoresPagesForNonPdfTextFile) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("component.tsx");
  ASSERT_TRUE(base::WriteFile(file, "export const value = 1;\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.pages = "1-2";
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_NE(std::string::npos,
            result.value.GetString().find("1\texport const value = 1;"))
      << result.value.GetString();
}

TEST(FileToolTest, ReadRejectsUnsupportedBinaryFile) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("blob.bin");
  constexpr uint8_t kBinaryBytes[] = {0x00, 0x01, 0x02, 0x03, 0xff};
  ASSERT_TRUE(base::WriteFile(file, base::span(kBinaryBytes)));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("unsupported_binary_file", result.error_code);
}

TEST(FileToolTest, ReadRejectsNoLimitTextFileOverDefaultSizeCap) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("large.txt");
  std::string contents = "alpha\n";
  contents.append(256 * 1024, 'b');
  ASSERT_TRUE(base::WriteFile(file, contents));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_too_large", result.error_code);
  EXPECT_NE(std::string::npos,
            result.error_message.find("exceeds maximum allowed size"));
}

TEST(FileToolTest, ReadWithExplicitLimitCanTargetLargeTextFile) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("large.txt");
  std::string contents = "alpha\n";
  contents.append(256 * 1024, 'b');
  ASSERT_TRUE(base::WriteFile(file, contents));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_NE(std::string::npos, result.value.GetString().find("1\talpha"))
      << result.value.GetString();
}

TEST(FileToolTest, ReadRejectsOutputOverDefaultTokenCap) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("large.json");
  ASSERT_TRUE(base::WriteFile(file, std::string(60000, '1')));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("max_file_read_token_exceeded", result.error_code);
  EXPECT_NE(std::string::npos,
            result.error_message.find("exceeds maximum allowed tokens"));
}

// CC parity: an empty file must resolve with the system-reminder warning
// (FileReadTool emits it in place of file content), not an empty result.
TEST(FileToolTest, ReadEmptyFileWarnsInsteadOfEmptyResult) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("empty.txt");
  ASSERT_TRUE(base::WriteFile(file, ""));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_EQ(
      "<system-reminder>Warning: the file exists but the contents are "
      "empty.</system-reminder>",
      result.value.GetString());
}

// CC parity: an offset past EOF reports the file length rather than
// returning an empty result.
TEST(FileToolTest, ReadOffsetBeyondEofWarnsWithFileLength) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("short.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.offset = 100;
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_EQ(
      "<system-reminder>Warning: the file exists but is shorter than the "
      "provided offset (100). The file has 3 lines.</system-reminder>",
      result.value.GetString());
}

// CC parity: reading an empty file records the read state, so a follow-up
// Write to that (still empty) file is not wrongly rejected as "not read yet".
TEST(FileToolTest, WriteAfterReadingEmptyFileSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("empty.txt");
  ASSERT_TRUE(base::WriteFile(file, ""));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::ReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  CallResult write_result;
  ::nexus::generated::WriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "beta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(write_result.resolved);
  EXPECT_FALSE(write_result.rejected);
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(file, &contents));
  EXPECT_EQ("beta\n", contents);
}

// CC parity (readFileInRange strips a leading U+FEFF): the model-facing
// projection must not carry the BOM bytes on line 1.
TEST(FileToolTest, ReadStripsLeadingUtf8Bom) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("bom.json");
  ASSERT_TRUE(base::WriteFile(file, "\xEF\xBB\xBF{\"key\": 1}\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result.resolved);
  ASSERT_TRUE(result.value.is_string());
  const std::string& projection = result.value.GetString();
  // Line 1 content begins immediately after the tab — no BOM glued in.
  EXPECT_NE(std::string::npos, projection.find("1\t{\"key\": 1}")) << projection;
  // The raw BOM bytes must not survive anywhere in the projection.
  EXPECT_EQ(std::string::npos, projection.find("\xEF\xBB\xBF")) << projection;
}

// CC parity (readFileInRange throws EISDIR): a directory Read is reported
// distinctly so the model switches to a listing tool instead of retrying.
TEST(FileToolTest, ReadDirectoryReturnsEisdir) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath dir = temp_dir.GetPath().AppendASCII("subdir");
  ASSERT_TRUE(base::CreateDirectory(dir));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = dir.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("is_directory", result.error_code);
  EXPECT_NE(std::string::npos, result.error_message.find("EISDIR"))
      << result.error_message;
}

// CC parity (FileReadTool ENOENT branch): a missing file gets a distinct
// "File does not exist." signal, not an opaque "failed to read file".
TEST(FileToolTest, ReadMissingFileReturnsFileNotFound) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("nope.txt");

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_not_found", result.error_code);
  EXPECT_NE(std::string::npos, result.error_message.find("File does not exist"))
      << result.error_message;
}

TEST(FileToolTest, EditRequiresPriorFullRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::EditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "alpha";
  input.new_string = "gamma";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_not_read", result.error_code);
}

TEST(FileToolTest, EditAfterPartialReadReportsSpecificErrorMessage) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha
beta
gamma
"));

  // Partial read (limit=1)
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::ReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  read_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  // Edit should be rejected with a specific partial-read message
  CallResult edit_result;
  ::nexus::generated::EditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "beta";
  edit_input.new_string = "delta";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(edit_result.resolved);
  EXPECT_TRUE(edit_result.rejected);
  EXPECT_EQ("file_not_read", edit_result.error_code);
  EXPECT_NE(std::string::npos, edit_result.error_message.find("partially read"));
  EXPECT_NE(std::string::npos, edit_result.error_message.find("offset"));
  EXPECT_NE(std::string::npos, edit_result.error_message.find("limit"));
  EXPECT_NE(std::string::npos, edit_result.error_message.find("full file"));
}

TEST(FileToolTest, WriteAfterPartialReadReportsSpecificErrorMessage) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha
beta
gamma
"));

  // Partial read (offset=2, limit=1)
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::ReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  read_input.offset = 2;
  read_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  // Write should be rejected with a specific partial-read message
  CallResult write_result;
  ::nexus::generated::WriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "delta
";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(write_result.resolved);
  EXPECT_TRUE(write_result.rejected);
  EXPECT_EQ("file_not_read", write_result.error_code);
  EXPECT_NE(std::string::npos, write_result.error_message.find("partially read"));
  EXPECT_NE(std::string::npos, write_result.error_message.find("offset: 2"));
  EXPECT_NE(std::string::npos, write_result.error_message.find("limit: 1"));
  EXPECT_NE(std::string::npos, write_result.error_message.find("full file"));
}

TEST(FileToolTest, EditAfterReadSucceedsAndUpdatesReadState) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::ReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  CallResult edit_result;
  ::nexus::generated::EditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "alpha";
  edit_input.new_string = "gamma";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(edit_result.resolved);
  EXPECT_FALSE(edit_result.rejected);
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(file, &contents));
  EXPECT_EQ("gamma\nbeta\n", contents);

  CallResult second_edit_result;
  ::nexus::generated::EditInput second_edit_input;
  second_edit_input.file_path = file.AsUTF8Unsafe();
  second_edit_input.old_string = "beta";
  second_edit_input.new_string = "delta";
  session.file_tool().RunEditOnUI(std::move(second_edit_input),
                                  MakeRecordingCallback(&second_edit_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(second_edit_result.resolved);
  EXPECT_FALSE(second_edit_result.rejected);
}

TEST(FileToolTest, WriteExistingFileRequiresPriorRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::WriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "beta\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_not_read", result.error_code);
}

TEST(FileToolTest, WriteNewFileDoesNotRequirePriorRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("new.txt");

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::WriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "beta\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(file, &contents));
  EXPECT_EQ("beta\n", contents);
}

TEST(FileToolTest, WriteRejectsFileModifiedSinceRead) {
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::ReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  task_environment.FastForwardBy(base::Seconds(1));
  ASSERT_TRUE(base::WriteFile(file, "external\n"));

  CallResult write_result;
  ::nexus::generated::WriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "beta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(write_result.resolved);
  EXPECT_TRUE(write_result.rejected);
  EXPECT_EQ("file_modified_since_read", write_result.error_code);
}

// `chrome.dll!nexus::FileTool::Run...OnThreadPool` (access violation reading
// 0xefefefef…, the freed-memory poison, during an atomic refcount bump).
//
// Root cause: a per-session FileTool method was posted to base::ThreadPool
// bound with base::Unretained(category). If the session was torn down (sidebar
// delete / panel close) before the worker ran, the worker dereferenced the
// freed FileTool.
//
// The fix runs orchestration on UI and hops only the *static* `<Name>Blocking`
// helper (no `this`) to the pool, binding the reply through
// weak_ptr_factory_.GetWeakPtr(). Read still uses that thread-pool path (Glob
// moved to the ripgrep shell service), so it exercises the fix: destroying the
// session mid-I/O must NOT crash, and the discarded ToolCallback must surface
// `tool_dropped` rather than resolving into freed memory.
TEST(FileToolTest, DestroyingSessionMidReadDropsReplyWithoutUseAfterFree) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\n"));

  CallResult result;
  auto session =
      std::make_unique<ToolSession>(/*profile=*/nullptr, kSessionKey);
  session->set_cwd(temp_dir.GetPath());
  ::nexus::generated::ReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session->file_tool().RunReadOnUI(std::move(input),
                                   MakeRecordingCallback(&result));

  // Tear the session down while ReadFileBlocking is still queued on the pool.
  // ~ToolSession → ~FileTool → OnStop(kDestructor) → InvalidateWeakPtrs(), so
  // the pending reply's WeakPtr is invalidated before it runs.
  session.reset();

  // Drains the pool task (static ReadFileBlocking — no FileTool to touch) and
  // the reply (dropped: the WeakPtr is dead). Before the fix this dereferenced
  // freed memory on the worker; now it must not crash.
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("tool_dropped", result.error_code);
}

// Glob's absolute-pattern → (searchRoot, relativePattern) split, a faithful
// port of claude_code's extractGlobBaseDirectory (utils/glob.ts). RunGlobOnUI
// applies it to absolute patterns so ripgrep gets a concrete search root + a
// relative `--glob` tail (rg's --glob is relative-only). Pure logic; the rg
// exec itself is covered by the nexus-tools e2e spec.
TEST(FileToolTest, GlobExtractsBaseDirectoryFromAbsolutePattern) {
  using Split = std::pair<std::string, std::string>;

  // A literal path splits into (dirname, basename).
  EXPECT_EQ(Split("/a/b/c", "file.cc"),
            FileTool::ExtractGlobBaseDirectory("/a/b/c/file.cc"));
  // Backslash separators are normalized before splitting.
  EXPECT_EQ(Split("/a/b/c", "file.cc"),
            FileTool::ExtractGlobBaseDirectory("\\a\\b\\c\\file.cc"));

  // A wildcard segment splits at the last separator before the first glob
  // metacharacter; the tail keeps the wildcard.
  EXPECT_EQ(Split("/src", "**/*.cc"),
            FileTool::ExtractGlobBaseDirectory("/src/**/*.cc"));
  EXPECT_EQ(Split("/a/b", "*.txt"),
            FileTool::ExtractGlobBaseDirectory("/a/b/*.txt"));

  // Windows drive root: "C:" is drive-relative, so the search root is "C:/".
  EXPECT_EQ(Split("C:/", "*.txt"),
            FileTool::ExtractGlobBaseDirectory("C:/*.txt"));
  EXPECT_EQ(Split("C:/src", "*.cc"),
            FileTool::ExtractGlobBaseDirectory("C:/src/*.cc"));

  // Leading-slash root collapses baseDir to "/".
  EXPECT_EQ(Split("/", "*.txt"), FileTool::ExtractGlobBaseDirectory("/*.txt"));

  // No separator before the glob (or a bare relative name): empty baseDir, so
  // the whole pattern stays as the tail (rg searches `path`/cwd verbatim).
  EXPECT_EQ(Split("", "*.ts"), FileTool::ExtractGlobBaseDirectory("*.ts"));
  EXPECT_EQ(Split("", "**/*.ts"),
            FileTool::ExtractGlobBaseDirectory("**/*.ts"));
  EXPECT_EQ(Split("", "config.json"),
            FileTool::ExtractGlobBaseDirectory("config.json"));
}

// Regression for the 0x80000003 crash at
// `chrome!nexus::NexusRipgrepRunner::Run`: once rg stdout exceeds
// BigBuffer::kMaxInlineBytes, Mojo backs it with shared memory. The runner must
// read it through the storage-agnostic BigBuffer span conversion, not
// BigBuffer::byte_span() (which CHECKs on shared-memory-backed buffers).
TEST(NexusRipgrepRunnerTest, ParsesSharedMemoryBackedStdout) {
  std::string text;
  while (text.size() <= mojo_base::BigBuffer::kMaxInlineBytes) {
    text.append("src/custom_browser/browser/nexus/tools/file.cc\n");
  }

  std::vector<uint8_t> bytes(text.begin(), text.end());
  auto result = mojom::ShellExecResult::New();
  result->exit = mojom::ShellExitInfo::New();
  result->exit->code = 0;
  result->stdout_buffer = mojo_base::BigBuffer(base::span(bytes));
  result->stderr_buffer = mojo_base::BigBuffer();

  ASSERT_EQ(result->stdout_buffer.storage_type(),
            mojo_base::BigBuffer::StorageType::kSharedMemory);

  RipgrepRunResult parsed = NexusRipgrepRunner::ParseExecResultForTesting(
      std::move(result), text.size());

  EXPECT_TRUE(parsed.launched);
  EXPECT_EQ(0, parsed.exit_code);
  EXPECT_GE(parsed.lines.size(), 2u);
  EXPECT_EQ("src/custom_browser/browser/nexus/tools/file.cc",
            parsed.lines.front());
}

}  // namespace

// CC parity (FileReadTool records readFileState for notebooks): a notebook
// Read must establish native read-state so a subsequent Write/Edit on the
// .ipynb passes the read precondition. The live notebook path crosses a
// utility-process boundary the unit-test harness can't spin up, so this drives
// the media-reply trampoline (OnReadMediaDone) directly with the same result
// shape the service produces. Defined in the named `nexus` namespace (not the
// anonymous one above) so the FRIEND_TEST_ALL_PREFIXES grant in file_tool.h —
// which names nexus::FileToolTest_…_Test — actually matches this test class.
TEST(FileToolTest, NotebookReadRecordsReadStateEnablingWrite) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("nb.ipynb");
  const std::string notebook_json = "{\"cells\": []}";
  ASSERT_TRUE(base::WriteFile(file, notebook_json));

  ToolSession session(/*profile=*/nullptr, kSessionKey);

  auto media = mojom::MediaReadResult::New();
  media->full_text_for_cache = notebook_json;
  auto part = mojom::MediaPart::New();
  part->type = "text";
  part->text = "<cell id=\"c0\"></cell>";
  media->parts.push_back(std::move(part));

  CallResult read_result;
  session.file_tool().OnReadMediaDone(file, MakeRecordingCallback(&read_result),
                                      /*ok=*/true, std::move(media),
                                      std::nullopt, std::nullopt);
  ASSERT_TRUE(read_result.resolved);

  CallResult write_result;
  ::nexus::generated::WriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "{\"cells\": [{\"cell_type\": \"code\"}]}";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(write_result.resolved)
      << write_result.error_code << ": " << write_result.error_message;
  EXPECT_FALSE(write_result.rejected);
}

}  // namespace nexus
