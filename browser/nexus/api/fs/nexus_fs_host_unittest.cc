// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "build/build_config.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include "base/test/file_path_reparse_point_win.h"
#endif

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

#if NEXUS_FS_ATOMIC_WRITE
// `atomic` writes go through base::ImportantFileWriter: a temp file in the
// target's directory, then a replace. The guest asks for this on the session
// `meta.json` (the sidebar's one-object projection), whose unlogged fields
// have no other copy. Pins: the target carries exactly the new bytes, an
// existing file is replaced (not appended to, not left as a mix), and no
// `*.tmp` is left behind on the happy path. The default (no flag) stays the
// in-place write.
TEST(NexusFsHostTest, WriteFileAtomicReplacesTargetAndLeavesNoTempFile) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  const base::FilePath target = temp_dir.GetPath().AppendASCII("meta.json");
  ASSERT_TRUE(base::WriteFile(target, "{\"old\":true,\"padding\":\"xxxxxxxx\"}"));

  auto write = [&](const std::string& text, bool atomic) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    return host->WriteFile("meta.json", mojo_base::BigBuffer(base::span(bytes)),
                           atomic);
  };

  NexusFsHost::SimpleResult atomic = write("{\"new\":1}", /*atomic=*/true);
  EXPECT_TRUE(atomic.success) << atomic.error;
  std::string content;
  ASSERT_TRUE(base::ReadFileToString(target, &content));
  EXPECT_EQ("{\"new\":1}", content);

  // Nothing but the target in the directory — the temp file was replaced
  // into place, not left beside it.
  base::FileEnumerator files(temp_dir.GetPath(), /*recursive=*/false,
                             base::FileEnumerator::FILES);
  int count = 0;
  for (base::FilePath p = files.Next(); !p.empty(); p = files.Next()) {
    ++count;
    EXPECT_EQ(target, p);
  }
  EXPECT_EQ(1, count);

  // The default is still the in-place write, same result on the happy path.
  NexusFsHost::SimpleResult plain = write("{\"new\":2}", /*atomic=*/false);
  EXPECT_TRUE(plain.success) << plain.error;
  ASSERT_TRUE(base::ReadFileToString(target, &content));
  EXPECT_EQ("{\"new\":2}", content);
}

// Regression: an `atomic` write must reach as deep as a plain one.
//
// base::ImportantFileWriter names its temp file with a 36-char GUID + ".tmp",
// so it needs 41 characters of headroom past the target's DIRECTORY and fails
// ~32 characters before the target path itself hits MAX_PATH. A background
// sub-agent's `meta.json` lives at
//   projects/<sanitized-project-root>/<parentSessionId>/subagents/<subId>/
// which crossed that line on a normally-nested profile: the atomic write
// failed, and because it is the sub-agent's first write the whole sub-agent
// aborted with "Failed to write file" — while a plain write to the same path
// would have succeeded. Pinned by building a directory in exactly that band:
// short enough that the target is writable, deep enough that a GUID temp name
// beside it would not be.
TEST(NexusFsHostTest, WriteFileAtomicSucceedsOnDeeplyNestedTarget) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  // The target is `<deep dir>/meta.json`. Aim the DIRECTORY at the band where
  // a 40-char temp name overflows but the 9-char target does not, measured
  // from the real temp root so this holds wherever the test runs.
  constexpr size_t kMaxPath = 260;
  constexpr size_t kGuidTempNameLen = 40;  // 36-char GUID + ".tmp"
  // `\<guid>.tmp` is 41 chars, so the directory must be at least
  // kMaxPath - kGuidTempNameLen for the temp name to overflow; `\meta.json` is
  // 10, so it must stay at or below kMaxPath - 11 for the target to be
  // writable. Anything in that band exercises the gap.
  const size_t min_dir_len = kMaxPath - kGuidTempNameLen;
  const size_t max_dir_len = kMaxPath - 11;
  ASSERT_LT(temp_dir.GetPath().value().length(), min_dir_len)
      << "temp root too long to build the deep-path case";

  base::FilePath deep = temp_dir.GetPath();
  std::string relative;
  while (deep.value().length() < min_dir_len) {
    deep = deep.AppendASCII("nested");
    relative += "nested/";
  }
  ASSERT_LE(deep.value().length(), max_dir_len)
      << "overshot the band — the target itself would be too long";
  ASSERT_TRUE(base::CreateDirectory(deep));
  ASSERT_LT(deep.value().length() + 10, kMaxPath)
      << "the target itself must be writable — otherwise this tests nothing";
  ASSERT_GT(deep.value().length() + 1 + kGuidTempNameLen, kMaxPath)
      << "a GUID temp name must overflow — otherwise this tests nothing";

  const std::string text = "{\"deep\":true}";
  std::vector<uint8_t> bytes(text.begin(), text.end());
  NexusFsHost::SimpleResult result = host->WriteFile(
      relative + "meta.json", mojo_base::BigBuffer(base::span(bytes)),
      /*atomic=*/true);
  EXPECT_TRUE(result.success) << result.error;

  std::string content;
  ASSERT_TRUE(
      base::ReadFileToString(deep.AppendASCII("meta.json"), &content));
  EXPECT_EQ(text, content);

  // The fallback must still clean up after itself.
  base::FileEnumerator files(deep, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  int count = 0;
  for (base::FilePath p = files.Next(); !p.empty(); p = files.Next()) {
    ++count;
    EXPECT_EQ(deep.AppendASCII("meta.json"), p);
  }
  EXPECT_EQ(1, count);
}
#endif  // NEXUS_FS_ATOMIC_WRITE

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

// Regression (fs error taxonomy, 2026-09-01): a missing path's Stat must
// report the discriminated ENOENT wording, not the retired ambiguous
// "File not found or permission denied". The JS guest keys "genuinely
// absent" off the leading code so SuperEdit's commit conflict check can
// tell a missing file from a transient permission/lock/I/O failure —
// which must NOT read as absent (see nexus_web utils/fsTransportFailure.ts).
TEST(NexusFsHostTest, StatMissingReportsEnoent) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::StatResult result = host->Stat("does-not-exist.txt");
  EXPECT_FALSE(result.success);
  // EXACT equality pins the MECHANISM, not just the string: since the
  // review-round fix removed the default arm's existence-probe fallback,
  // the FILE_ERROR_NOT_FOUND switch arm is the ONLY producer of this
  // wording — so this assertion fails if GetLastFileError capture ever
  // goes dead (a missing path would then read EUNKNOWN).
  EXPECT_EQ("ENOENT: no such file or directory", result.error);
}

// Pins every arm of the wire-message taxonomy directly — the guest's
// isFsNotFoundError regex and its EACCES/EBUSY handling are calibrated
// against these exact strings, and platform-specific failures (locks,
// ACLs) cannot be staged portably in a unit test. Also pins that the
// unmapped default NEVER emits chromium's ErrorToString tokens (a
// chromium roll must not change guest-visible wire text) and NEVER
// matches the guest's not-found prose.
TEST(NexusFsHostTest, FsFailureMessageWireContract) {
  EXPECT_EQ("ENOENT: no such file or directory",
            FsFailureMessage(base::File::FILE_ERROR_NOT_FOUND));
  EXPECT_EQ("EACCES: permission denied",
            FsFailureMessage(base::File::FILE_ERROR_ACCESS_DENIED));
  EXPECT_EQ("EBUSY: file is in use or locked",
            FsFailureMessage(base::File::FILE_ERROR_IN_USE));
  EXPECT_EQ("ENOTDIR: not a directory",
            FsFailureMessage(base::File::FILE_ERROR_NOT_A_DIRECTORY));
  EXPECT_EQ("EIO: input/output error",
            FsFailureMessage(base::File::FILE_ERROR_IO));
  // Unmapped errors: raw enum value, transient-shaped, no enum tokens.
  const std::string unmapped =
      FsFailureMessage(base::File::FILE_ERROR_NO_SPACE);
  EXPECT_EQ(0u, unmapped.find("EUNKNOWN:")) << unmapped;
  EXPECT_EQ(std::string::npos, unmapped.find("FILE_ERROR")) << unmapped;
  EXPECT_EQ(std::string::npos, unmapped.find("ENOENT")) << unmapped;
  const std::string failed = FsFailureMessage(base::File::FILE_ERROR_FAILED);
  EXPECT_EQ(0u, failed.find("EUNKNOWN:")) << failed;
}

// Happy path: Stat on an existing file succeeds and reports a file, so the
// taxonomy change cannot have disturbed the success shape.
TEST(NexusFsHostTest, StatExistingFileStillSucceeds) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("f.txt"), "x"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::StatResult result = host->Stat("f.txt");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_NE(std::string::npos, result.json.find("\"isFile\":true"))
      << result.json;
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

// ---------------------------------------------------------------------------
// Batched routes. These exist to collapse the session-list scan from ~550
// bridge round-trips to ~5; the tests below pin the properties that make that
// safe rather than just fast.
// ---------------------------------------------------------------------------

namespace {

// `root/p1/s1/{transcript.jsonl, meta.json, tool-details/{a,b}.json}` — the
// shape of a real session store, including the directory that must NOT be
// descended into.
void BuildSessionStoreFixture(const base::FilePath& root) {
  const base::FilePath session =
      root.AppendASCII("p1").AppendASCII("s1");
  ASSERT_TRUE(base::CreateDirectory(session.AppendASCII("tool-details")));
  ASSERT_TRUE(base::WriteFile(session.AppendASCII("transcript.jsonl"),
                              "{\"type\":\"user\"}\n"));
  ASSERT_TRUE(base::WriteFile(session.AppendASCII("meta.json"),
                              "{\"schemaVersion\":1,\"sessionId\":\"s1\"}"));
  ASSERT_TRUE(base::WriteFile(
      session.AppendASCII("tool-details").AppendASCII("a.json"), "{}"));
  ASSERT_TRUE(base::WriteFile(
      session.AppendASCII("tool-details").AppendASCII("b.json"), "{}"));
}

bool HasRowWithPath(const base::ListValue& entries, const std::string& path) {
  for (const base::Value& row : entries) {
    const std::string* value = row.GetDict().FindString("path");
    if (value && *value == path) {
      return true;
    }
  }
  return false;
}

bool AnyRowPathStartsWith(const base::ListValue& entries,
                          const std::string& prefix) {
  for (const base::Value& row : entries) {
    const std::string* value = row.GetDict().FindString("path");
    if (value && value->rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

// THE depth-cap test. Without it a naive FileEnumerator(recursive=true)
// implementation passes every other test here while returning every
// tool-details file on a real profile — 24k rows on the author's.
TEST(NexusFsHostTest, ReaddirTreeStopsAtDepthCap) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  BuildSessionStoreFixture(temp_dir.GetPath());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReaddirTreeResult result = host->ReaddirTree(".", 3, 20000);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_FALSE(result.truncated);

  // The session's own files are reached at depth 3 …
  EXPECT_TRUE(HasRowWithPath(result.entries, "p1/s1/transcript.jsonl"));
  EXPECT_TRUE(HasRowWithPath(result.entries, "p1/s1/meta.json"));
  // … and tool-details is LISTED as a directory …
  EXPECT_TRUE(HasRowWithPath(result.entries, "p1/s1/tool-details"));
  // … but never descended into.
  EXPECT_FALSE(AnyRowPathStartsWith(result.entries, "p1/s1/tool-details/"));
}

// Row paths are a WIRE CONTRACT: root-relative and '/'-separated on every
// platform. On Windows a backslash here would make the guest's split yield a
// single segment, and the sidebar would go silently empty.
TEST(NexusFsHostTest, ReaddirTreeRelativePathsUseForwardSlash) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  BuildSessionStoreFixture(temp_dir.GetPath());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReaddirTreeResult result = host->ReaddirTree(".", 3, 20000);
  ASSERT_TRUE(result.success) << result.error;
  for (const base::Value& row : result.entries) {
    const std::string* path = row.GetDict().FindString("path");
    ASSERT_TRUE(path);
    EXPECT_EQ(std::string::npos, path->find('\\')) << *path;
  }
}

// The size on a tree row is what licenses dropping the per-session fs.stat —
// it supplies both the summary-cache key and the tail-read offset.
TEST(NexusFsHostTest, ReaddirTreeReportsSizeForFilesAndZeroForDirs) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("nine.txt"),
                              "123456789"));
  ASSERT_TRUE(base::CreateDirectory(temp_dir.GetPath().AppendASCII("sub")));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReaddirTreeResult result = host->ReaddirTree(".", 1, 20000);
  ASSERT_TRUE(result.success) << result.error;

  for (const base::Value& row : result.entries) {
    const base::DictValue& dict = row.GetDict();
    const std::string* path = dict.FindString("path");
    ASSERT_TRUE(path);
    if (*path == "nine.txt") {
      EXPECT_FALSE(dict.FindBool("isDirectory").value_or(true));
      EXPECT_EQ(9.0, dict.FindDouble("size").value_or(-1));
      EXPECT_TRUE(dict.FindDouble("mtimeMs").has_value());
    } else if (*path == "sub") {
      EXPECT_TRUE(dict.FindBool("isDirectory").value_or(false));
      EXPECT_EQ(0.0, dict.FindDouble("size").value_or(-1));
    }
  }
}

TEST(NexusFsHostTest, ReaddirTreeTruncatesAtMaxEntries) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(base::WriteFile(
        temp_dir.GetPath().AppendASCII(base::NumberToString(i) + ".txt"), "x"));
  }

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReaddirTreeResult result = host->ReaddirTree(".", 1, 2);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(2u, result.entries.size());
}

TEST(NexusFsHostTest, ReaddirTreeMissingRootIsAnError) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  // Distinct from "empty profile": the guest must be able to tell a failed
  // scan from a real zero-session answer, because the two lead to different
  // boot behaviour.
  NexusFsHost::ReaddirTreeResult result = host->ReaddirTree("nope", 3, 20000);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

TEST(NexusFsHostTest, StatBatchReturnsRowPerPathInOrderWithNullForMissing) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "abc"));
  ASSERT_TRUE(base::CreateDirectory(temp_dir.GetPath().AppendASCII("d")));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  // A duplicate is preserved, not deduped — callers index by position.
  const std::vector<std::string> paths = {"a.txt", "d", "missing.txt", "a.txt"};
  NexusFsHost::StatBatchResult result = host->StatBatch(paths);
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_EQ(4u, result.rows.size());

  for (size_t i = 0; i < paths.size(); ++i) {
    const base::DictValue& row = result.rows[i].GetDict();
    const std::string* echoed = row.FindString("path");
    ASSERT_TRUE(echoed);
    // The REQUEST string, not the resolved absolute path.
    EXPECT_EQ(paths[i], *echoed);
  }

  EXPECT_TRUE(result.rows[0].GetDict().FindDict("stat"));
  EXPECT_TRUE(result.rows[1]
                  .GetDict()
                  .FindDict("stat")
                  ->FindBool("isDirectory")
                  .value_or(false));
  // Absent is an ordinary answer, not a failure.
  EXPECT_TRUE(result.rows[2].GetDict().Find("stat")->is_none());
}

TEST(NexusFsHostTest, ReadBatchConcatenatesAndIndexesEachItem) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaa"));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("b.txt"), "bbbb"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadBatchResult result = host->ReadBatch(
      {{"a.txt", 0, 0}, {"b.txt", 0, 0}}, 1024 * 1024, 1024 * 1024);
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_TRUE(result.data.has_value());
  ASSERT_EQ(2u, result.entries.size());

  const std::string blob(reinterpret_cast<const char*>(result.data->data()),
                         result.data->size());
  EXPECT_EQ("aaabbbb", blob);

  const base::DictValue& first = result.entries[0].GetDict();
  EXPECT_TRUE(first.FindBool("ok").value_or(false));
  EXPECT_EQ(0.0, first.FindDouble("byteOffset").value_or(-1));
  EXPECT_EQ(3.0, first.FindDouble("byteLength").value_or(-1));

  const base::DictValue& second = result.entries[1].GetDict();
  EXPECT_EQ(3.0, second.FindDouble("byteOffset").value_or(-1));
  EXPECT_EQ(4.0, second.FindDouble("byteLength").value_or(-1));
}

// The migration path: a session with no `meta.json` yet is served by a 64KB
// transcript tail range in the SAME batch as the `meta.json` reads (the guest
// derives and writes its `meta.json` from it). Per-item ranges exist for this.
TEST(NexusFsHostTest, ReadBatchHonorsPerItemRange) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(
      base::WriteFile(temp_dir.GetPath().AppendASCII("t.jsonl"), "0123456789"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadBatchResult result =
      host->ReadBatch({{"t.jsonl", 7, 3}}, 1024 * 1024, 1024 * 1024);
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_TRUE(result.data.has_value());
  const std::string blob(reinterpret_cast<const char*>(result.data->data()),
                         result.data->size());
  EXPECT_EQ("789", blob);
}

// One bad path must not cost the other 225 sessions their summaries.
TEST(NexusFsHostTest, ReadBatchMarksMissingItemFailedAndKeepsTheRest) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaa"));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("c.txt"), "cc"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadBatchResult result = host->ReadBatch(
      {{"a.txt", 0, 0}, {"gone.txt", 0, 0}, {"c.txt", 0, 0}}, 1024 * 1024,
      1024 * 1024);
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_EQ(3u, result.entries.size());

  EXPECT_TRUE(result.entries[0].GetDict().FindBool("ok").value_or(false));
  EXPECT_FALSE(result.entries[1].GetDict().FindBool("ok").value_or(true));
  // The ENOENT wording survives the batch, same as fs.readFile.
  const std::string* error = result.entries[1].GetDict().FindString("error");
  ASSERT_TRUE(error);
  EXPECT_NE(std::string::npos, error->find("ENOENT")) << *error;
  EXPECT_TRUE(result.entries[2].GetDict().FindBool("ok").value_or(false));

  // The failed item contributes no bytes, and the survivor's offset accounts
  // for that — an off-by-one here would attribute bytes to the wrong session.
  const std::string blob(reinterpret_cast<const char*>(result.data->data()),
                         result.data->size());
  EXPECT_EQ("aaacc", blob);
  EXPECT_EQ(3.0, result.entries[2].GetDict().FindDouble("byteOffset").value_or(-1));
}

// TOCTOU: the index scan reports a size, and by the time the batch reads, the
// file has shrunk / been emptied / been deleted. None of those may fail the
// batch — the affected item comes back empty or ok:false and the guest simply
// skips that session. Rare in practice, but a whole sidebar must never be lost
// to one file changing underneath the scan.
TEST(NexusFsHostTest, ReadBatchToleratesFilesChangingAfterTheScan) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  // Scan saw 10 bytes; the file is 4 by the time we read a "tail" at offset 6.
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("shrunk.jsonl"),
                              "abcd"));
  ASSERT_TRUE(
      base::WriteFile(temp_dir.GetPath().AppendASCII("empty.jsonl"), ""));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("ok.jsonl"), "ok"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadBatchResult result = host->ReadBatch(
      {
          {"shrunk.jsonl", /*offset=*/6, /*length=*/64},  // offset past EOF now
          {"empty.jsonl", 0, 0},
          {"vanished.jsonl", /*offset=*/6, /*length=*/64},  // deleted
          {"ok.jsonl", 0, 0},
      },
      1024 * 1024, 1024 * 1024);

  // The CALL succeeds regardless.
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_EQ(4u, result.entries.size());

  // Past-EOF and empty are ordinary empty reads, not errors.
  EXPECT_TRUE(result.entries[0].GetDict().FindBool("ok").value_or(false));
  EXPECT_EQ(0.0, result.entries[0].GetDict().FindDouble("byteLength").value_or(-1));
  EXPECT_TRUE(result.entries[1].GetDict().FindBool("ok").value_or(false));
  EXPECT_EQ(0.0, result.entries[1].GetDict().FindDouble("byteLength").value_or(-1));
  // A vanished file is a per-item failure.
  EXPECT_FALSE(result.entries[2].GetDict().FindBool("ok").value_or(true));

  // …and the healthy item that follows them is unaffected, at the right offset.
  EXPECT_TRUE(result.entries[3].GetDict().FindBool("ok").value_or(false));
  EXPECT_EQ(0.0, result.entries[3].GetDict().FindDouble("byteOffset").value_or(-1));
  const std::string blob(reinterpret_cast<const char*>(result.data->data()),
                         result.data->size());
  EXPECT_EQ("ok", blob);
}

// A partial answer beats none for a sidebar; the guest re-requests the rest.
TEST(NexusFsHostTest, ReadBatchStopsAtByteBudgetWithoutFailingTheCall) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaaa"));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("b.txt"), "bbbb"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::ReadBatchResult result =
      host->ReadBatch({{"a.txt", 0, 0}, {"b.txt", 0, 0}}, /*max_total=*/4,
                      /*max_item=*/1024);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_TRUE(result.truncated);
  EXPECT_TRUE(result.entries[0].GetDict().FindBool("ok").value_or(false));
  EXPECT_FALSE(result.entries[1].GetDict().FindBool("ok").value_or(true));
}

TEST(NexusFsHostTest, RenameMovesFileWithExactBytes) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  // Explicit length: the literal embeds a NUL, so a bare const char* would
  // truncate at it.
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("old.bin"),
                              std::string("payload\x00\xff", 9)));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::SimpleResult result = host->Rename("old.bin", "sub/new.bin");
  // The destination's parent is NOT auto-created — parity with base::Move.
  EXPECT_FALSE(result.success);

  ASSERT_TRUE(base::CreateDirectory(temp_dir.GetPath().AppendASCII("sub")));
  result = host->Rename("old.bin", "sub/new.bin");
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_FALSE(base::PathExists(temp_dir.GetPath().AppendASCII("old.bin")));
  std::string content;
  ASSERT_TRUE(base::ReadFileToString(
      temp_dir.GetPath().AppendASCII("sub").AppendASCII("new.bin"), &content));
  EXPECT_EQ(std::string("payload\x00\xff", 9), content);
}

TEST(NexusFsHostTest, RenameMovesDirectoryTreeWhole) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath src = temp_dir.GetPath().AppendASCII("srcdir");
  ASSERT_TRUE(base::CreateDirectory(src.AppendASCII("nested")));
  ASSERT_TRUE(base::WriteFile(src.AppendASCII("nested").AppendASCII("f.txt"),
                              "deep"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::SimpleResult result = host->Rename("srcdir", "dstdir");
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_FALSE(base::PathExists(src));
  std::string content;
  ASSERT_TRUE(base::ReadFileToString(temp_dir.GetPath()
                                         .AppendASCII("dstdir")
                                         .AppendASCII("nested")
                                         .AppendASCII("f.txt"),
                                     &content));
  EXPECT_EQ("deep", content);
}

// The two refusal strings are wire contract: the JS guest maps them to
// NotFoundError / AlreadyExistsError by exact text (BrowserFsOperations).
TEST(NexusFsHostTest, RenameRefusalErrorStringsAreWireContract) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "a"));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("b.txt"), "b"));

  auto host = base::MakeRefCounted<NexusFsHost>(temp_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::SimpleResult missing = host->Rename("absent.txt", "c.txt");
  EXPECT_FALSE(missing.success);
  EXPECT_EQ("Source does not exist", missing.error);

  NexusFsHost::SimpleResult occupied = host->Rename("a.txt", "b.txt");
  EXPECT_FALSE(occupied.success);
  EXPECT_EQ("Destination already exists", occupied.error);
  // Refusals must leave both files untouched.
  std::string content;
  ASSERT_TRUE(base::ReadFileToString(temp_dir.GetPath().AppendASCII("a.txt"),
                                     &content));
  EXPECT_EQ("a", content);
  ASSERT_TRUE(base::ReadFileToString(temp_dir.GetPath().AppendASCII("b.txt"),
                                     &content));
  EXPECT_EQ("b", content);
}

// `resolveLinks` follows a link inside a directory that points out of it —
// the escape the untrusted read boundary (F-20) and the fsx root rule check
// for. On Windows the default form only normalizes, so a junction under a
// root still reads as under the root; this test fails there without the
// flag's implementation. Also pins: a path that does not exist yet resolves
// through its existing ancestor, and the default form is unchanged (it is the
// session cwd / storage identity).
TEST(NexusFsHostTest, RealpathResolveLinksFollowsALinkOutOfTheDirectory) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir root_dir;
  base::ScopedTempDir outside_dir;
  ASSERT_TRUE(root_dir.CreateUniqueTempDir());
  ASSERT_TRUE(outside_dir.CreateUniqueTempDir());
  ASSERT_TRUE(
      base::WriteFile(outside_dir.GetPath().AppendASCII("secret.txt"), "s"));
  ASSERT_TRUE(
      base::WriteFile(root_dir.GetPath().AppendASCII("plain.txt"), "p"));
  const base::FilePath link = root_dir.GetPath().AppendASCII("link");
#if BUILDFLAG(IS_WIN)
  // A junction: creatable without privileges, unlike a symlink. Declared
  // after both temp dirs so it is removed before they are deleted.
  ASSERT_TRUE(base::CreateDirectory(link));
  std::optional<base::test::FilePathReparsePoint> junction =
      base::test::FilePathReparsePoint::Create(link, outside_dir.GetPath());
  ASSERT_TRUE(junction.has_value());
#else
  ASSERT_TRUE(base::CreateSymbolicLink(outside_dir.GetPath(), link));
#endif
  // The temp dirs themselves may sit under an 8.3 or linked %TEMP%, so the
  // expectations are built from the resolved forms.
  base::FilePath real_root;
  base::FilePath real_outside;
  ASSERT_TRUE(base::NormalizeFilePath(root_dir.GetPath(), &real_root));
  ASSERT_TRUE(base::NormalizeFilePath(outside_dir.GetPath(), &real_outside));

  auto host = base::MakeRefCounted<NexusFsHost>(root_dir.GetPath());
  base::RunLoop().RunUntilIdle();

  NexusFsHost::RealpathResult through_link =
      host->Realpath("link/secret.txt", /*resolve_links=*/true);
  ASSERT_TRUE(through_link.success) << through_link.error;
  EXPECT_EQ(real_outside.AppendASCII("secret.txt").AsUTF8Unsafe(),
            through_link.path);

  NexusFsHost::RealpathResult link_itself =
      host->Realpath(link.AsUTF8Unsafe(), /*resolve_links=*/true);
  ASSERT_TRUE(link_itself.success) << link_itself.error;
  EXPECT_EQ(real_outside.AsUTF8Unsafe(), link_itself.path);

  NexusFsHost::RealpathResult not_yet =
      host->Realpath("link/new/file.txt", /*resolve_links=*/true);
  ASSERT_TRUE(not_yet.success) << not_yet.error;
  EXPECT_EQ(
      real_outside.AppendASCII("new").AppendASCII("file.txt").AsUTF8Unsafe(),
      not_yet.path);

  NexusFsHost::RealpathResult no_link =
      host->Realpath("plain.txt", /*resolve_links=*/true);
  ASSERT_TRUE(no_link.success) << no_link.error;
  EXPECT_EQ(real_root.AppendASCII("plain.txt").AsUTF8Unsafe(), no_link.path);

#if BUILDFLAG(IS_WIN)
  // The default form is untouched: normalized, the junction not followed.
  NexusFsHost::RealpathResult default_form =
      host->Realpath("link/secret.txt", /*resolve_links=*/false);
  ASSERT_TRUE(default_form.success) << default_form.error;
  EXPECT_EQ(base::MakeAbsoluteFilePath(link.AppendASCII("secret.txt"))
                .AsUTF8Unsafe(),
            default_form.path);
#endif
}

// ---- copyFile / symlink / readlink / chmod / writeFile ifMatch ------------

scoped_refptr<NexusFsHost> MakeHost(const base::FilePath& cwd) {
  auto host = base::MakeRefCounted<NexusFsHost>(cwd);
  base::RunLoop().RunUntilIdle();  // the ctor's CreateDirectory hop
  return host;
}

mojo_base::BigBuffer Bytes(const std::string& text) {
  std::vector<uint8_t> bytes(text.begin(), text.end());
  return mojo_base::BigBuffer(base::span(bytes));
}

TEST(NexusFsHostTest, CopyFileRefusesAnExistingDestinationUnlessOverwrite) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto host = MakeHost(temp_dir.GetPath());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaa"));

  EXPECT_TRUE(host->CopyFile("a.txt", "b.txt", false).success);
  std::string copied;
  ASSERT_TRUE(base::ReadFileToString(temp_dir.GetPath().AppendASCII("b.txt"),
                                     &copied));
  EXPECT_EQ(copied, "aaa");

  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "new"));
  NexusFsHost::SimpleResult refused = host->CopyFile("a.txt", "b.txt", false);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(refused.error.rfind("EEXIST:", 0), 0u) << refused.error;
  EXPECT_TRUE(host->CopyFile("a.txt", "b.txt", true).success);
  ASSERT_TRUE(base::ReadFileToString(temp_dir.GetPath().AppendASCII("b.txt"),
                                     &copied));
  EXPECT_EQ(copied, "new");

  NexusFsHost::SimpleResult missing = host->CopyFile("nope", "c.txt", false);
  EXPECT_FALSE(missing.success);
  EXPECT_EQ(missing.error.rfind("ENOENT:", 0), 0u) << missing.error;
  EXPECT_FALSE(host->CopyFile(".", "d", false).success);  // a directory
}

// copyFile opens both ends as readFile / writeFile do, so it fails the way
// they fail: a `..` step is refused as EACCES (base::File), not with
// whatever error an earlier call left behind (base::CopyFile returned early
// without setting one). And a file copied onto itself is refused, not
// truncated.
TEST(NexusFsHostTest, CopyFileFailsLikeReadAndWriteAndRefusesItself) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto host = MakeHost(temp_dir.GetPath());
  ASSERT_TRUE(base::CreateDirectory(temp_dir.GetPath().AppendASCII("sub")));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaa"));

  NexusFsHost::SimpleResult from_parent =
      host->CopyFile("sub/../a.txt", "b.txt", false);
  EXPECT_FALSE(from_parent.success);
  EXPECT_EQ(from_parent.error, "EACCES: permission denied");
  NexusFsHost::SimpleResult to_parent =
      host->CopyFile("a.txt", "sub/../b.txt", false);
  EXPECT_FALSE(to_parent.success);
  EXPECT_EQ(to_parent.error, "Failed to copy file (EACCES: permission denied)");

  NexusFsHost::SimpleResult itself = host->CopyFile("a.txt", "./a.txt", true);
  EXPECT_FALSE(itself.success);
  EXPECT_EQ(itself.error.rfind("EINVAL:", 0), 0u) << itself.error;
  std::string content;
  ASSERT_TRUE(base::ReadFileToString(temp_dir.GetPath().AppendASCII("a.txt"),
                                     &content));
  EXPECT_EQ(content, "aaa");
}

TEST(NexusFsHostTest, WriteFileIfMatchWritesOnlyWhileTheStatMatches) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto host = MakeHost(temp_dir.GetPath());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("doc.txt");
  ASSERT_TRUE(base::WriteFile(file, "v1"));
  base::File::Info info;
  ASSERT_TRUE(base::GetFileInfo(file, &info));

  NexusFsHost::WriteIfMatch seen;
  seen.size = static_cast<double>(info.size);
  seen.mtime_ms = info.last_modified.InMillisecondsFSinceUnixEpoch();
  NexusFsHost::WriteIfMatchResult ok =
      host->WriteFileIfMatch("doc.txt", Bytes("v2-longer"), false, seen);
  ASSERT_TRUE(ok.success) << ok.error;
  EXPECT_EQ(ok.size, 9);
  std::string content;
  ASSERT_TRUE(base::ReadFileToString(file, &content));
  EXPECT_EQ(content, "v2-longer");

  // The stale stat (size 2) no longer matches: nothing is written and the
  // code is the distinct one the guest branches on.
  NexusFsHost::WriteIfMatchResult stale =
      host->WriteFileIfMatch("doc.txt", Bytes("v3"), false, seen);
  EXPECT_FALSE(stale.success);
  EXPECT_EQ(stale.error_code, NexusFsHost::kPreconditionFailedCode);
  ASSERT_TRUE(base::ReadFileToString(file, &content));
  EXPECT_EQ(content, "v2-longer");

  // The stat the successful write returned is the next ifMatch.
  NexusFsHost::WriteIfMatch next;
  next.size = ok.size;
  next.mtime_ms = ok.mtime_ms;
  EXPECT_TRUE(
      host->WriteFileIfMatch("doc.txt", Bytes("v3"), false, next).success);

  NexusFsHost::WriteIfMatchResult gone =
      host->WriteFileIfMatch("missing.txt", Bytes("x"), false, next);
  EXPECT_FALSE(gone.success);
  EXPECT_EQ(gone.error_code, NexusFsHost::kPreconditionFailedCode);
  EXPECT_FALSE(base::PathExists(temp_dir.GetPath().AppendASCII("missing.txt")));
}

TEST(NexusFsHostTest, ChmodSetsBitsOnPosixAndIsANoOpOnWindows) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto host = MakeHost(temp_dir.GetPath());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("run.sh"), "x"));

  NexusFsHost::ChmodResult result = host->Chmod("run.sh", 0700);
  ASSERT_TRUE(result.success) << result.error;
#if BUILDFLAG(IS_POSIX)
  EXPECT_TRUE(result.applied);
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(
      temp_dir.GetPath().AppendASCII("run.sh"), &mode));
  EXPECT_EQ(mode & 0777, 0700);
#else
  EXPECT_FALSE(result.applied);
#endif
  NexusFsHost::ChmodResult missing = host->Chmod("nope", 0644);
  EXPECT_FALSE(missing.success);
  EXPECT_EQ(missing.error.rfind("ENOENT:", 0), 0u) << missing.error;
}

TEST(NexusFsHostTest, SymlinkThenReadlinkRoundTrips) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto host = MakeHost(temp_dir.GetPath());
  const base::FilePath target = temp_dir.GetPath().AppendASCII("target.txt");
  ASSERT_TRUE(base::WriteFile(target, "t"));

  NexusFsHost::SimpleResult made =
      host->Symlink(target.AsUTF8Unsafe(), "link.txt");
#if BUILDFLAG(IS_WIN)
  // Without Developer Mode or elevation Windows refuses — with the
  // documented EPERM wording, not a generic failure.
  if (!made.success) {
    EXPECT_EQ(made.error.rfind("EPERM:", 0), 0u) << made.error;
    return;
  }
#endif
  ASSERT_TRUE(made.success) << made.error;
  NexusFsHost::ReadlinkResult read = host->Readlink("link.txt");
  ASSERT_TRUE(read.success) << read.error;
  base::FilePath normalized_target;
  ASSERT_TRUE(base::NormalizeFilePath(target, &normalized_target));
  base::FilePath normalized_read;
  ASSERT_TRUE(base::NormalizeFilePath(base::FilePath::FromUTF8Unsafe(read.target),
                                      &normalized_read));
  EXPECT_EQ(normalized_read, normalized_target);

  // An existing path is refused; a plain file is not a link.
  NexusFsHost::SimpleResult again =
      host->Symlink(target.AsUTF8Unsafe(), "link.txt");
  EXPECT_FALSE(again.success);
  EXPECT_EQ(again.error.rfind("EEXIST:", 0), 0u) << again.error;
  NexusFsHost::ReadlinkResult plain = host->Readlink("target.txt");
  EXPECT_FALSE(plain.success);
  EXPECT_EQ(plain.error.rfind("EINVAL:", 0), 0u) << plain.error;
}

}  // namespace
}  // namespace nexus
