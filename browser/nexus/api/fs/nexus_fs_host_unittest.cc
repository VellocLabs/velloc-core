// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_host.h"

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

}  // namespace
}  // namespace nexus
