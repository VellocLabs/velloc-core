// Copyright 2026 VellocLabs
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
#include "custom_browser/browser/nexus/api/files/files_api.gen.h"
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
  base::DictValue error_params;
};

ToolCallback MakeRecordingCallback(CallResult* out) {
  auto on_success = base::BindLambdaForTesting([out](base::Value value) {
    out->resolved = true;
    out->value = std::move(value);
  });
  auto on_error = base::BindLambdaForTesting(
      [out](std::string code, std::string message, base::DictValue params) {
        out->rejected = true;
        out->error_code = std::move(code);
        out->error_message = std::move(message);
        out->error_params = std::move(params);
      });
  return ToolCallback(std::move(on_success), std::move(on_error));
}

// Spec 2-1-5 errorCode 9: Read refuses the POSIX devices a read could never
// finish — endless output, interactive streams, and stdio descriptors 0-2
// under /dev/fd/ and /proc/<pid|self>/fd/ — and nothing next to them.
TEST(FileToolTest, BlockedDevicePathsAreExactlyTheUnfinishableOnes) {
  for (const char* path :
       {"/dev/zero", "/dev/full", "/dev/random", "/dev/urandom", "/dev/tty",
        "/dev/console", "/dev/stdin", "/dev/stdout", "/dev/stderr",
        "/dev/fd/0", "/dev/fd/1", "/dev/fd/2", "/proc/self/fd/0",
        "/proc/self/fd/1", "/proc/self/fd/2", "/proc/123/fd/2"}) {
    EXPECT_TRUE(IsBlockedDevicePathForTesting(path)) << path;
  }
  for (const char* path :
       {"/dev/null", "/dev/fd/3", "/dev/fd/00", "/proc/self/fd/0/",
        "/proc/self/fdx/1", "/dev/zero2", "/dev/", "/dev/fd/",
        "/proc/self/fd/3", "/proc/self/status", "/tmp/dev/zero",
        "dev/zero"}) {
    EXPECT_FALSE(IsBlockedDevicePathForTesting(path)) << path;
  }
}

// Happy path: Read hops the blocking I/O to base::ThreadPool and resolves
// back on UI with the line-numbered projection (spec 2-1-7).
TEST(FileToolTest, ReadResolvesWithLineNumberedProjection) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
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

// Spec 2-1-9 / 2-1-10: the 2000-line figure is description guidance and NOT
// enforced. A no-limit Read returns the whole file regardless of context
// window; overflow is caught by the token guard (which throws), not a line cap.
TEST(FileToolTest, ReadWithNoLimitReturnsWholeFile) {
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

  // Small context window — previously capped at 1000 lines; now returns all.
  ToolSession small_session(/*profile=*/nullptr, kSessionKey);
  CallResult small_result;
  ::nexus::generated::FilesReadInput small_input;
  small_input.file_path = file.AsUTF8Unsafe();
  small_session.file_tool().RunReadOnUI(std::move(small_input),
                                        MakeRecordingCallback(&small_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(small_result.resolved);
  ASSERT_TRUE(small_result.value.is_string());
  const std::string& small = small_result.value.GetString();
  EXPECT_NE(std::string::npos, small.find("2100\tline 2100")) << small;
  EXPECT_EQ(std::string::npos, small.find("[file truncated")) << small;

  // Large context window — same result (context window no longer scales the
  // line cap; the token budget is a fixed 25000).
  ToolSession large_session(/*profile=*/nullptr, kSessionKey);
  CallResult large_result;
  ::nexus::generated::FilesReadInput large_input;
  large_input.file_path = file.AsUTF8Unsafe();
  large_input.context_window = 500001;
  large_session.file_tool().RunReadOnUI(std::move(large_input),
                                        MakeRecordingCallback(&large_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(large_result.resolved);
  ASSERT_TRUE(large_result.value.is_string());
  const std::string& large = large_result.value.GetString();
  EXPECT_NE(std::string::npos, large.find("2100\tline 2100")) << large;
  EXPECT_EQ(std::string::npos, large.find("[file truncated")) << large;
}

TEST(FileToolTest, ReadDedupsUnchangedFullTextRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::FilesReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  CallResult second_result;
  ::nexus::generated::FilesReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_dict());
  const base::DictValue* notice = second_result.value.GetDict().FindDict("notice");
  ASSERT_TRUE(notice);
  EXPECT_EQ("unchanged", *notice->FindString("kind"));
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
  ::nexus::generated::FilesReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  task_environment.FastForwardBy(base::Seconds(1));
  ASSERT_TRUE(base::WriteFile(file, "gamma\n"));

  CallResult second_result;
  ::nexus::generated::FilesReadInput second_input;
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

// Spec 2-1-8 / 2-1-10 inv.4: Read never sets isPartialView, so a partial
// read (offset/limit) does NOT block a later Edit/Write — the staleness guard is
// the content comparison, not the read's range. Repeated exact-range reads still
// dedup to the unchanged stub.
TEST(FileToolTest, RepeatedPartialReadDedupsExactRangeAndAllowsEditWrite) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::FilesReadInput first_input;
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
  ::nexus::generated::FilesReadInput second_input;
  second_input.file_path = file.AsUTF8Unsafe();
  second_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(second_input),
                                  MakeRecordingCallback(&second_result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(second_result.resolved);
  ASSERT_TRUE(second_result.value.is_dict());
  EXPECT_EQ("unchanged", *second_result.value.GetDict().FindDict("notice")->FindString("kind"));

  // Spec 2-1-8: a partial read does NOT block Edit (no isPartialView).
  CallResult edit_result;
  ::nexus::generated::FilesEditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "beta";
  edit_input.new_string = "delta";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();
  EXPECT_TRUE(edit_result.resolved)
      << edit_result.error_code << ": " << edit_result.error_message;
  EXPECT_FALSE(edit_result.rejected);

  // Spec 2-1-8: a partial read does NOT block Write either.
  CallResult write_result;
  ::nexus::generated::FilesWriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "delta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();
  EXPECT_TRUE(write_result.resolved)
      << write_result.error_code << ": " << write_result.error_message;
  EXPECT_FALSE(write_result.rejected);
}

TEST(FileToolTest, PartialReadFollowedByFullReadDoesNotDedup) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult first_result;
  ::nexus::generated::FilesReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  first_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  CallResult second_result;
  ::nexus::generated::FilesReadInput second_input;
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
  ::nexus::generated::FilesReadInput first_input;
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
  ::nexus::generated::FilesReadInput same_range_input;
  same_range_input.file_path = file.AsUTF8Unsafe();
  same_range_input.offset = 2;
  same_range_input.limit = 2;
  session.file_tool().RunReadOnUI(std::move(same_range_input),
                                  MakeRecordingCallback(&same_range_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(same_range_result.resolved);
  ASSERT_TRUE(same_range_result.value.is_dict());
  EXPECT_EQ("unchanged", *same_range_result.value.GetDict().FindDict("notice")->FindString("kind"));

  CallResult different_limit_result;
  ::nexus::generated::FilesReadInput different_limit_input;
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
  ::nexus::generated::FilesReadInput different_offset_input;
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
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  CallResult edit_result;
  ::nexus::generated::FilesEditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "alpha";
  edit_input.new_string = "gamma";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(edit_result.resolved);

  CallResult post_edit_read_result;
  ::nexus::generated::FilesReadInput post_edit_read_input;
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
  ::nexus::generated::FilesReadInput second_post_edit_read_input;
  second_post_edit_read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(second_post_edit_read_input),
                                  MakeRecordingCallback(&second_post_edit_read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(second_post_edit_read_result.resolved);
  ASSERT_TRUE(second_post_edit_read_result.value.is_dict());
  EXPECT_EQ("unchanged", *second_post_edit_read_result.value.GetDict().FindDict("notice")->FindString("kind"));

  CallResult write_result;
  ::nexus::generated::FilesWriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "delta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(write_result.resolved);

  CallResult post_write_read_result;
  ::nexus::generated::FilesReadInput post_write_read_input;
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
  ::nexus::generated::FilesReadInput first_input;
  first_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(first_input),
                                  MakeRecordingCallback(&first_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first_result.resolved);

  session.file_tool().ClearReadState();

  CallResult second_result;
  ::nexus::generated::FilesReadInput second_input;
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
  ::nexus::generated::FilesReadInput input;
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

// Spec 2-1-5 errorCode 7: a malformed `pages` string is rejected
// before any I/O.
TEST(FileToolTest, ReadRejectsInvalidPagesFormat) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("doc.pdf");
  ASSERT_TRUE(base::WriteFile(file, "%PDF-1.4\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.pages = "abc";
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("invalid_pages", result.error_code);
  EXPECT_TRUE(result.error_params.FindString("pages"));
}

// Spec 2-1-5 errorCode 8: a page range wider than 20 pages is
// rejected before any I/O.
TEST(FileToolTest, ReadRejectsTooManyPages) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("doc.pdf");
  ASSERT_TRUE(base::WriteFile(file, "%PDF-1.4\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.pages = "1-99";
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("too_many_pages", result.error_code);
  EXPECT_EQ(20, result.error_params.FindInt("max_pages").value_or(0));
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
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("unsupported_binary_file", result.error_code);
}

// Spec 2-1-6: a UTF-16LE file (BOM FF FE) is decoded
// to UTF-8 for the projection, NOT rejected as binary despite its null bytes.
TEST(FileToolTest, ReadDecodesUtf16LeInsteadOfRejectingAsBinary) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("utf16.txt");
  // UTF-16LE BOM + "hi\n" in little-endian code units.
  const std::string utf16_content("\xFF\xFE"
                                  "h\x00i\x00\n\x00", 8);
  ASSERT_TRUE(base::WriteFile(file, utf16_content));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  ASSERT_TRUE(result.value.is_string());
  // The projection shows the decoded UTF-8 text with line numbers.
  EXPECT_NE(std::string::npos, result.value.GetString().find("1\thi"))
      << result.value.GetString();
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
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_too_large", result.error_code);
  EXPECT_GT(result.error_params.FindDouble("size_bytes").value_or(0),
            result.error_params.FindDouble("limit_bytes").value_or(0));
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
  ::nexus::generated::FilesReadInput input;
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
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("max_file_read_token_exceeded", result.error_code);
  EXPECT_GT(result.error_params.FindInt("tokens").value_or(0),
            result.error_params.FindInt("max_tokens").value_or(0));
}

// Spec 2-1-6: an empty file must resolve with the system-reminder warning
// in place of file content, not an empty result.
TEST(FileToolTest, ReadEmptyFileWarnsInsteadOfEmptyResult) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("empty.txt");
  ASSERT_TRUE(base::WriteFile(file, ""));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_dict());
  const base::DictValue* notice = result.value.GetDict().FindDict("notice");
  ASSERT_TRUE(notice);
  EXPECT_EQ("empty", *notice->FindString("kind"));
}

// Spec 2-1-6: an offset past EOF reports the file length rather than
// returning an empty result.
TEST(FileToolTest, ReadOffsetBeyondEofWarnsWithFileLength) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("short.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.offset = 100;
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved);
  EXPECT_FALSE(result.rejected);
  ASSERT_TRUE(result.value.is_dict());
  const base::DictValue* notice = result.value.GetDict().FindDict("notice");
  ASSERT_TRUE(notice);
  EXPECT_EQ("short", *notice->FindString("kind"));
  EXPECT_EQ(100, notice->FindInt("offset").value_or(0));
  EXPECT_EQ(3, notice->FindInt("total_lines").value_or(0));
}

// Spec 2-1-8: reading an empty file records the read state, so a follow-up
// Write to that (still empty) file is not wrongly rejected as "not read yet".
TEST(FileToolTest, WriteAfterReadingEmptyFileSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("empty.txt");
  ASSERT_TRUE(base::WriteFile(file, ""));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  CallResult write_result;
  ::nexus::generated::FilesWriteInput write_input;
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

// Spec 2-1-6 (a leading U+FEFF is stripped): the model-facing
// projection must not carry the BOM bytes on line 1.
TEST(FileToolTest, ReadStripsLeadingUtf8Bom) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("bom.json");
  ASSERT_TRUE(base::WriteFile(file, "\xEF\xBB\xBF{\"key\": 1}\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
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

// Spec 2-1-5: a directory Read is reported
// distinctly so the model switches to a listing tool instead of retrying.
TEST(FileToolTest, ReadDirectoryReturnsEisdir) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath dir = temp_dir.GetPath().AppendASCII("subdir");
  ASSERT_TRUE(base::CreateDirectory(dir));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = dir.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("is_directory", result.error_code);
  EXPECT_TRUE(result.error_params.FindString("path"));
}

// Spec 2-1-5 row 5: a missing file gets a distinct
// "File does not exist." signal, not an opaque "failed to read file".
TEST(FileToolTest, ReadMissingFileReturnsFileNotFound) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("nope.txt");

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_not_found", result.error_code);
}

TEST(FileToolTest, EditRequiresPriorFullRead) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesEditInput input;
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

// Spec 2-1-8 / 2-1-10 inv.4: a partial read does NOT block Edit. The
// read-state content snapshot is the full file (Read caches full_content), so
// the staleness guard passes and the edit applies.
TEST(FileToolTest, EditAfterPartialReadSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  // Partial read (limit=1)
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  read_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  // Edit succeeds — a partial read is not a partial view (spec 2-1-8).
  CallResult edit_result;
  ::nexus::generated::FilesEditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "beta";
  edit_input.new_string = "delta";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(edit_result.resolved)
      << edit_result.error_code << ": " << edit_result.error_message;
  EXPECT_FALSE(edit_result.rejected);
}

// Spec 2-1-8 / 2-1-10 inv.4: a partial read does NOT block Write.
TEST(FileToolTest, WriteAfterPartialReadSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  // Partial read (offset=2, limit=1)
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  read_input.offset = 2;
  read_input.limit = 1;
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  // Write succeeds — a partial read is not a partial view (spec 2-1-8).
  CallResult write_result;
  ::nexus::generated::FilesWriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "delta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(write_result.resolved)
      << write_result.error_code << ": " << write_result.error_message;
  EXPECT_FALSE(write_result.rejected);
}

TEST(FileToolTest, EditAfterTruncatedReadSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("big.txt");
  std::string content;
  for (int i = 1; i <= 3000; ++i) {
    content += "line " + base::NumberToString(i) + "\n";
  }
  ASSERT_TRUE(base::WriteFile(file, content));

  // Read with no offset/limit — will be truncated at 2000 lines
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  // Edit should succeed because truncated read is NOT a partial view
  CallResult edit_result;
  ::nexus::generated::FilesEditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "line 1500";
  edit_input.new_string = "edited line 1500";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(edit_result.resolved);
  EXPECT_FALSE(edit_result.rejected);
}

TEST(FileToolTest, WriteAfterTruncatedReadSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("big.txt");
  std::string content;
  for (int i = 1; i <= 3000; ++i) {
    content += "line " + base::NumberToString(i) + "\n";
  }
  ASSERT_TRUE(base::WriteFile(file, content));

  // Read with no offset/limit — will be truncated at 2000 lines
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  // Write should succeed because truncated read is NOT a partial view
  CallResult write_result;
  ::nexus::generated::FilesWriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "completely new content\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(write_result.resolved);
  EXPECT_FALSE(write_result.rejected);
}

TEST(FileToolTest, EditAfterReadSucceedsAndUpdatesReadState) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  CallResult edit_result;
  ::nexus::generated::FilesEditInput edit_input;
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
  ::nexus::generated::FilesEditInput second_edit_input;
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
  ::nexus::generated::FilesWriteInput input;
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
  ::nexus::generated::FilesWriteInput input;
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
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved);

  task_environment.FastForwardBy(base::Seconds(1));
  ASSERT_TRUE(base::WriteFile(file, "external\n"));

  CallResult write_result;
  ::nexus::generated::FilesWriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "beta\n";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(write_result.resolved);
  EXPECT_TRUE(write_result.rejected);
  EXPECT_EQ("file_modified_since_read", write_result.error_code);
}

// Destroying the session mid-read must not use-after-free the FileTool, and
// the dropped ToolCallback must surface `tool_dropped`.
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
  ::nexus::generated::FilesReadInput input;
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

// Glob's absolute-pattern → (searchRoot, relativePattern) split. RunGlobOnUI
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

// Spec 2-1-8: a notebook
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
  ::nexus::generated::FilesWriteInput write_input;
  write_input.file_path = file.AsUTF8Unsafe();
  write_input.content = "{\"cells\": [{\"cell_type\": \"code\"}]}";
  session.file_tool().RunWriteOnUI(std::move(write_input),
                                   MakeRecordingCallback(&write_result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(write_result.resolved)
      << write_result.error_code << ": " << write_result.error_message;
  EXPECT_FALSE(write_result.rejected);
}

// =============================================================================
// Edit regression tests (spec 2-2)
// =============================================================================

// Helper: seed a full read so EditFileBlocking passes the read gate.
static void SeedReadState(ToolSession& session,
                          base::test::TaskEnvironment& env,
                          const base::FilePath& file) {
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  env.RunUntilIdle();
  ASSERT_TRUE(read_result.resolved) << "seed read failed";
}

// Spec 2-2-4 code 5: .ipynb → reject with NotebookEdit hint.
TEST(FileToolTest, EditRejectsNotebookFile) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("nb.ipynb");
  ASSERT_TRUE(base::WriteFile(file, "{\"cells\":[]}"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "cells";
  input.new_string = "blocks";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("notebook_file", result.error_code);
}

// Spec 2-2-4 code 4: missing file + non-empty old_string → file_not_found.
TEST(FileToolTest, EditMissingFileWithOldStringReportsNotFound) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("ghost.txt");

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "hello";
  input.new_string = "world";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_not_found", result.error_code);
}

// Spec 2-2-4 code 7 (creation): missing file + empty old_string → creates file.
TEST(FileToolTest, EditCreatesNewFileWithEmptyOldString) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("new.txt");

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "";
  input.new_string = "hello world";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("hello world", written);
}

// Spec 2-2-4 code 3: existing non-empty file + empty old_string → error.
TEST(FileToolTest, EditEmptyOldStringOnNonEmptyFileRejects) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("existing.txt");
  ASSERT_TRUE(base::WriteFile(file, "content"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "";
  input.new_string = "new content";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("file_already_exists", result.error_code);
}

// Spec 2-2-8: CRLF file edited → stays CRLF.
TEST(FileToolTest, EditPreservesCRLFLineEndings) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("crlf.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\r\nbeta\r\ngamma\r\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "beta";
  input.new_string = "delta";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("alpha\r\ndelta\r\ngamma\r\n", written);
}

// Spec 2-2-6: delete (new_string='') of a full line → trailing newline removed.
TEST(FileToolTest, EditDeleteStripsTrailingNewline) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("lines.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "beta";
  input.new_string = "";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("alpha\ngamma\n", written);
}

// Spec 2-2-6: curly quotes in file, straight quotes from model → match +
// preserve curly style in new_string.
TEST(FileToolTest, EditNormalizesCurlyQuotesAndPreservesStyle) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("curly.txt");
  // U+201C "hello" U+201D in UTF-8.
  ASSERT_TRUE(base::WriteFile(
      file, "\xE2\x80\x9Chello\xE2\x80\x9D world\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "\"hello\"";  // straight quotes from model
  input.new_string = "\"goodbye\"";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  // new_string should have curly quotes applied.
  EXPECT_EQ("\xE2\x80\x9Cgoodbye\xE2\x80\x9D world\n", written);
}

// FindActualString regression: curly quotes BEFORE the match. In UTF-8 the
// quote normalization shrinks 3-byte curly quotes to 1 byte, so a normalized
// match position does not line up with the original bytes once any curly
// quote precedes it. The old code indexed file_content with the normalized
// position and replaced a shifted region (corrupting the file); the offset
// map in FindActualString must translate both ends of the match back to the
// original byte range.
TEST(FileToolTest, EditQuoteNormalizedMatchAfterEarlierCurlyQuotes) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("curly2.txt");
  // “intro” says “target” end — curly quotes both before and inside the match.
  ASSERT_TRUE(base::WriteFile(
      file,
      "\xE2\x80\x9Cintro\xE2\x80\x9D says \xE2\x80\x9Ctarget\xE2\x80\x9D "
      "end\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "says \"target\"";  // straight quotes from model
  input.new_string = "says \"done\"";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  // The untouched “intro” prefix must survive and the replacement must carry
  // the file's curly style.
  EXPECT_EQ(
      "\xE2\x80\x9Cintro\xE2\x80\x9D says \xE2\x80\x9C"
      "done\xE2\x80\x9D end\n",
      written);
}

// Spec 2-2-6: trailing whitespace stripped from new_string for non-.md files.
TEST(FileToolTest, EditStripsTrailingWhitespaceForNonMarkdown) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("code.ts");
  ASSERT_TRUE(base::WriteFile(file, "const x = 1;\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "const x = 1;";
  input.new_string = "const x = 2;   ";  // trailing spaces
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("const x = 2;\n", written);
}

// Spec 2-2-6: .md trailing double-space preserved (hard line break).
TEST(FileToolTest, EditPreservesTrailingSpacesForMarkdown) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("doc.md");
  ASSERT_TRUE(base::WriteFile(file, "line one\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "line one";
  input.new_string = "line one  ";  // markdown hard break
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("line one  \n", written);
}

// Spec 2-2-4 code 9: multiple matches + replace_all:false → ambiguous_match.
TEST(FileToolTest, EditAmbiguousMatchReportsCount) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("dup.txt");
  ASSERT_TRUE(base::WriteFile(file, "foo bar foo baz foo\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "foo";
  input.new_string = "qux";
  input.replace_all = false;
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("ambiguous_match", result.error_code);
  EXPECT_EQ(3, result.error_params.FindInt("count").value_or(0));
}

// Spec 2-2-4 code 9: multiple matches + replace_all:true → replaces all.
TEST(FileToolTest, EditReplaceAllReplacesEveryOccurrence) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("dup.txt");
  ASSERT_TRUE(base::WriteFile(file, "foo bar foo baz foo\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "foo";
  input.new_string = "qux";
  input.replace_all = true;
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("qux bar qux baz qux\n", written);
  EXPECT_EQ("updated_all", *result.value.GetDict().FindString("outcome"));
}

// Spec 2-2-4 code 1: old_string === new_string → no-op error.
TEST(FileToolTest, EditNoOpRejects) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("noop.txt");
  ASSERT_TRUE(base::WriteFile(file, "same\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "same";
  input.new_string = "same";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("invalid_args", result.error_code);
  EXPECT_EQ("same_strings", *result.error_params.FindString("reason"));
}

// Spec 2-2-6.9: desanitize — old has the sanitized `<e>`, file has `<error>`
// → match; same replacement applied to new_string. (`<e>`/`</e>` are clean
// tokens with no substring overlap, so both open and close desanitize and
// mirror onto new_string deterministically.)
TEST(FileToolTest, EditDesanitizesMatchAndNewString) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("desan.txt");
  ASSERT_TRUE(base::WriteFile(file, "<error>oops</error>\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "<e>oops</e>";  // sanitized form from model
  input.new_string = "<e>fixed</e>";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("<error>fixed</error>\n", written);
}

// Spec 2-3-7.1: Write ALWAYS outputs LF, even if the model sends CRLF.
TEST(FileToolTest, WriteNormalizesCRLFToLF) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("crlf.txt");
  ASSERT_TRUE(base::WriteFile(file, "line1\r\nline2\r\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  // Model sends CRLF content — Write must normalize to LF on disk.
  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "new1\r\nnew2\r\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("new1\nnew2\n", written);
}

// Spec 2-3-7.2: Write preserves UTF-8 BOM from the original file.
TEST(FileToolTest, WritePreservesUtf8Bom) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("bom.txt");
  const std::string bom_content = "\xEF\xBB\xBFhello\n";
  ASSERT_TRUE(base::WriteFile(file, bom_content));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "world\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("\xEF\xBB\xBFworld\n", written);
}

// Spec 2-3-7.2: Write preserves UTF-16LE encoding (BOM-detected) from the
// original file. Read now decodes UTF-16LE (spec 2-1-6), so SeedReadState
// can seed the read_state_ cache and Write can re-encode on output.
TEST(FileToolTest, WritePreservesUtf16LeEncoding) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("utf16.txt");
  // UTF-16LE BOM + "hello\n" in little-endian code units.
  const std::string utf16_content(
      "\xFF\xFE"
      "h\x00e\x00l\x00l\x00o\x00\n\x00", 14);
  ASSERT_TRUE(base::WriteFile(file, utf16_content));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "world\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  // Expect UTF-16LE BOM + "world\n" in little-endian.
  const std::string expected(
      "\xFF\xFE"
      "w\x00o\x00r\x00l\x00" "d\x00\n\x00", 14);
  EXPECT_EQ(expected, written);
}

// Spec 2-2-8.2: Edit preserves UTF-16LE encoding (BOM-detected). Read decodes
// to UTF-8 for matching; Edit re-encodes to UTF-16LE on write.
TEST(FileToolTest, EditPreservesUtf16LeEncoding) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("utf16.txt");
  // UTF-16LE BOM + "hello\n" in little-endian code units.
  // NOTE: split before 'e' — \x00e would be parsed as \x{00e}=14.
  const std::string utf16_content(
      "\xFF\xFE"
      "h\x00" "e\x00l\x00l\x00o\x00\n\x00", 14);
  ASSERT_TRUE(base::WriteFile(file, utf16_content));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesEditInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.old_string = "hello";
  input.new_string = "world";
  session.file_tool().RunEditOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  // Expect UTF-16LE BOM + "world\n" in little-endian.
  const std::string expected(
      "\xFF\xFE"
      "w\x00o\x00r\x00l\x00" "d\x00\n\x00", 14);
  EXPECT_EQ(expected, written);
}

// Spec 2-3-4 item 3: UNC paths are rejected to prevent NTLM credential leaks.
TEST(FileToolTest, WriteRejectsUncPath) {
  base::test::TaskEnvironment task_environment;
  ToolSession session(/*profile=*/nullptr, kSessionKey);

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = "\\\\server\\share\\file.txt";
  input.content = "data";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("invalid_args", result.error_code);
  EXPECT_EQ("unc_path", *result.error_params.FindString("reason"));
}

// Spec 2-3-4 item 3: forward-slash UNC variant is also rejected.
TEST(FileToolTest, WriteRejectsForwardSlashUncPath) {
  base::test::TaskEnvironment task_environment;
  ToolSession session(/*profile=*/nullptr, kSessionKey);

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = "//server/share/file.txt";
  input.content = "data";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("invalid_args", result.error_code);
}

// Spec 2-3-5.2: overwriting an existing file renders the update message and
// includes original_file in the structured result.
TEST(FileToolTest, WriteUpdateRendersUpdateMessage) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("update.txt");
  ASSERT_TRUE(base::WriteFile(file, "old content\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "new content\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result.resolved) << result.error_message;
  ASSERT_TRUE(result.value.is_dict());
  const std::string* outcome = result.value.GetDict().FindString("outcome");
  ASSERT_TRUE(outcome);
  EXPECT_EQ("updated", *outcome);
  // original_file must be present for the update branch.
  EXPECT_TRUE(result.value.GetDict().FindString("original_file"));
}

// Spec 2-3-5.4: mtime advanced but content unchanged → Write passes.
// The local implementation uses content comparison (not mtime), so a
// same-content rewrite with a bumped mtime must succeed.
TEST(FileToolTest, WritePassesWhenContentUnchangedDespiteMtimeAdvance) {
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("same.txt");
  ASSERT_TRUE(base::WriteFile(file, "unchanged\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  // Bump mtime without changing content.
  task_environment.FastForwardBy(base::Seconds(5));
  ASSERT_TRUE(base::WriteFile(file, "unchanged\n"));

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "replaced\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("replaced\n", written);
}

// Spec 2-3-5.5: after Write, ReadState is updated so a subsequent Write
// without a fresh Read still passes (content matches what was written).
TEST(FileToolTest, WriteUpdatesReadStateForSubsequentWrite) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("chain.txt");
  ASSERT_TRUE(base::WriteFile(file, "v1\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  SeedReadState(session, task_environment, file);

  // First write.
  CallResult first;
  ::nexus::generated::FilesWriteInput input1;
  input1.file_path = file.AsUTF8Unsafe();
  input1.content = "v2\n";
  session.file_tool().RunWriteOnUI(std::move(input1),
                                   MakeRecordingCallback(&first));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(first.resolved) << first.error_message;

  // Second write without a fresh Read — must succeed because Write updated
  // ReadState with the content it wrote.
  CallResult second;
  ::nexus::generated::FilesWriteInput input2;
  input2.file_path = file.AsUTF8Unsafe();
  input2.content = "v3\n";
  session.file_tool().RunWriteOnUI(std::move(input2),
                                   MakeRecordingCallback(&second));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(second.resolved) << second.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("v3\n", written);
}

// Spec 2-3-5.6: writing to a path under a missing directory creates the
// directory tree and writes the file.
TEST(FileToolTest, WriteCreatesMissingParentDirectory) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file =
      temp_dir.GetPath().AppendASCII("newdir").AppendASCII("f.txt");

  ToolSession session(/*profile=*/nullptr, kSessionKey);

  CallResult result;
  ::nexus::generated::FilesWriteInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.content = "created\n";
  session.file_tool().RunWriteOnUI(std::move(input),
                                   MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_TRUE(result.resolved) << result.error_message;
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(file, &written));
  EXPECT_EQ("created\n", written);
}

// =============================================================================
// Read regression tests (spec 2-1)
// =============================================================================

// Spec 2-1-6.1: exact line-numbered content for "a\nb\nc".
TEST(FileToolTest, ReadExactLineNumberedContent) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("abc.txt");
  ASSERT_TRUE(base::WriteFile(file, "a\nb\nc\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result.resolved);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_EQ("1\ta\n2\tb\n3\tc\n", result.value.GetString());
}

// Spec 2-1-6.4: offset:0 is equivalent to offset:1 (both start at line 1).
TEST(FileToolTest, ReadOffsetZeroEquivalentToOffsetOne) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("lines.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session0(/*profile=*/nullptr, kSessionKey);
  CallResult result0;
  ::nexus::generated::FilesReadInput input0;
  input0.file_path = file.AsUTF8Unsafe();
  input0.offset = 0;
  session0.file_tool().RunReadOnUI(std::move(input0),
                                   MakeRecordingCallback(&result0));
  task_environment.RunUntilIdle();

  ToolSession session1(/*profile=*/nullptr, kSessionKey);
  CallResult result1;
  ::nexus::generated::FilesReadInput input1;
  input1.file_path = file.AsUTF8Unsafe();
  input1.offset = 1;
  session1.file_tool().RunReadOnUI(std::move(input1),
                                   MakeRecordingCallback(&result1));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result0.resolved);
  ASSERT_TRUE(result1.resolved);
  ASSERT_TRUE(result0.value.is_string());
  ASSERT_TRUE(result1.value.is_string());
  EXPECT_EQ(result0.value.GetString(), result1.value.GetString());
  EXPECT_NE(std::string::npos, result0.value.GetString().find("1\talpha"));
}

// Spec 2-1-6.5: CRLF line endings are stripped to LF in the projection.
TEST(FileToolTest, ReadStripsCrlfToLf) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("crlf.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\r\nbeta\r\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result.resolved);
  ASSERT_TRUE(result.value.is_string());
  const std::string& content = result.value.GetString();
  EXPECT_NE(std::string::npos, content.find("1\talpha\n"));
  EXPECT_NE(std::string::npos, content.find("2\tbeta\n"));
  // No \r should survive in the projection.
  EXPECT_EQ(std::string::npos, content.find('\r'));
}

// Spec 2-1-2.6: offset < 1 is clamped to 1 (reads from the beginning).
TEST(FileToolTest, ReadClampsNegativeOffsetToOne) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("lines.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.offset = -1;
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result.resolved);
  ASSERT_TRUE(result.value.is_string());
  EXPECT_NE(std::string::npos, result.value.GetString().find("1\talpha"));
}

// Spec 2-1-2.6: limit < 1 is treated as no limit (reads the whole file).
TEST(FileToolTest, ReadTreatsZeroLimitAsNoLimit) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("lines.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\ngamma\n"));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  input.limit = 0;
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  ASSERT_TRUE(result.resolved);
  ASSERT_TRUE(result.value.is_string());
  // All three lines should be present (limit=0 → no cap).
  EXPECT_NE(std::string::npos, result.value.GetString().find("1\talpha"));
  EXPECT_NE(std::string::npos, result.value.GetString().find("2\tbeta"));
  EXPECT_NE(std::string::npos, result.value.GetString().find("3\tgamma"));
}

// Spec 2-1-4.1: a file_too_large rejection does NOT set read state, so a
// subsequent Edit still gets file_not_read.
TEST(FileToolTest, ReadFileTooLargeDoesNotSetReadState) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("large.txt");
  std::string contents = "alpha\n";
  contents.append(256 * 1024, 'b');
  ASSERT_TRUE(base::WriteFile(file, contents));

  ToolSession session(/*profile=*/nullptr, kSessionKey);

  // Read fails with file_too_large.
  CallResult read_result;
  ::nexus::generated::FilesReadInput read_input;
  read_input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(read_input),
                                  MakeRecordingCallback(&read_result));
  task_environment.RunUntilIdle();
  ASSERT_TRUE(read_result.rejected);
  EXPECT_EQ("file_too_large", read_result.error_code);

  // Edit must still fail with file_not_read (read state was NOT set).
  CallResult edit_result;
  ::nexus::generated::FilesEditInput edit_input;
  edit_input.file_path = file.AsUTF8Unsafe();
  edit_input.old_string = "alpha";
  edit_input.new_string = "beta";
  session.file_tool().RunEditOnUI(std::move(edit_input),
                                  MakeRecordingCallback(&edit_result));
  task_environment.RunUntilIdle();
  EXPECT_TRUE(edit_result.rejected);
  EXPECT_EQ("file_not_read", edit_result.error_code);
}

// Spec 2-1-5.5: a .zip file (binary content) is rejected as unsupported binary.
TEST(FileToolTest, ReadRejectsZipAsBinary) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("archive.zip");
  // Minimal ZIP local file header (PK\x03\x04) — enough for binary detection.
  constexpr uint8_t kZipHeader[] = {0x50, 0x4B, 0x03, 0x04, 0x14, 0x00,
                                    0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  ASSERT_TRUE(base::WriteFile(file, base::span(kZipHeader)));

  ToolSession session(/*profile=*/nullptr, kSessionKey);
  CallResult result;
  ::nexus::generated::FilesReadInput input;
  input.file_path = file.AsUTF8Unsafe();
  session.file_tool().RunReadOnUI(std::move(input),
                                  MakeRecordingCallback(&result));
  task_environment.RunUntilIdle();

  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ("unsupported_binary_file", result.error_code);
}


// The ripgrep runner's pre-launch failure rejects Glob with the tool's own
// declared code; the runner's code is a param (see ProcessToolTest).
TEST(FileToolTest, RipgrepFailureRejectsGlobWithTheToolsDeclaredCode) {
  base::test::TaskEnvironment task_environment;
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  RipgrepRunResult rg;
  rg.launched = false;
  rg.error_code = "INVALID_CWD";
  rg.error_message = "the search directory does not exist";
  CallResult result;
  session.file_tool().CompleteGlobForTesting(MakeRecordingCallback(&result),
                                             std::move(rg));
  ASSERT_TRUE(result.rejected);
  EXPECT_EQ("glob_failed", result.error_code);
  EXPECT_TRUE(::nexus::generated::IsFilesErrorCode(result.error_code));
  EXPECT_EQ("INVALID_CWD", *result.error_params.FindString("service_code"));
  EXPECT_EQ("the search directory does not exist",
            *result.error_params.FindString("detail"));
}

}  // namespace nexus
