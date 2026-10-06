// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_watcher.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusFsWatchCoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    core_ = std::make_unique<NexusFsWatchCore>(base::BindRepeating(
        [](std::vector<FsWatchEvent>* out, FsWatchEvent event) {
          out->push_back(std::move(event));
        },
        &events_));
  }

  base::FilePath Path(const std::string& name) const {
    return temp_dir_.GetPath().AppendASCII(name);
  }

  // One poll pass, then the debounce window.
  void PollAndSettle(const std::string& watch_id) {
    core_->PollNowForTesting(watch_id);
    task_environment_.FastForwardBy(NexusFsWatchCore::kDebounce);
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_dir_;
  std::vector<FsWatchEvent> events_;
  std::unique_ptr<NexusFsWatchCore> core_;
};

TEST(NexusFsSnapshotTest, DiffReportsCreatedDeletedModifiedInPathOrder) {
  const base::FilePath a = base::FilePath::FromUTF8Unsafe("/r/a");
  const base::FilePath b = base::FilePath::FromUTF8Unsafe("/r/b");
  const base::FilePath c = base::FilePath::FromUTF8Unsafe("/r/c");
  FsSnapshot before;
  before[a] = FsSnapshotEntry{1, 10, false};
  before[b] = FsSnapshotEntry{2, 20, false};
  FsSnapshot after;
  after[b] = FsSnapshotEntry{3, 21, false};  // modified
  after[c] = FsSnapshotEntry{1, 30, false};  // created; `a` deleted

  const auto changes = DiffFsSnapshots(before, after);
  ASSERT_EQ(changes.size(), 3u);
  EXPECT_EQ(changes[0], std::make_pair(a, std::string("deleted")));
  EXPECT_EQ(changes[1], std::make_pair(b, std::string("modified")));
  EXPECT_EQ(changes[2], std::make_pair(c, std::string("created")));
  EXPECT_TRUE(DiffFsSnapshots(after, after).empty());
}

TEST(NexusFsSnapshotTest, MissingRootIsEmptyAndAFileIsItself) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  EXPECT_TRUE(TakeFsSnapshot(dir.GetPath().AppendASCII("nope"), true, 10,
                             nullptr)
                  .empty());
  const base::FilePath file = dir.GetPath().AppendASCII("f.txt");
  ASSERT_TRUE(base::WriteFile(file, "abc"));
  const FsSnapshot snapshot = TakeFsSnapshot(file, false, 10, nullptr);
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot.begin()->first, file);
  EXPECT_EQ(snapshot.begin()->second.size, 3);
}

TEST(NexusFsSnapshotTest, StopsAtTheEntryCap) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(base::WriteFile(
        dir.GetPath().AppendASCII("f" + base::NumberToString(i)), "x"));
  }
  bool truncated = false;
  EXPECT_EQ(TakeFsSnapshot(dir.GetPath(), false, 3, &truncated).size(), 3u);
  EXPECT_TRUE(truncated);
}

TEST(NexusFsSnapshotTest, KindNames) {
  using Type = base::FilePathWatcher::ChangeType;
  EXPECT_EQ(FsChangeKindName(Type::kCreated), "created");
  EXPECT_EQ(FsChangeKindName(Type::kDeleted), "deleted");
  EXPECT_EQ(FsChangeKindName(Type::kModified), "modified");
  EXPECT_EQ(FsChangeKindName(Type::kMoved), "moved");
  EXPECT_EQ(FsChangeKindName(Type::kUnknown), "change");
}

TEST_F(NexusFsWatchCoreTest, PollWatchReportsACreatedFileOnce) {
  const auto added = core_->Add("w1", temp_dir_.GetPath(), false, true);
  ASSERT_TRUE(added.ok);
  EXPECT_EQ(added.mode, "poll");

  ASSERT_TRUE(base::WriteFile(Path("new.txt"), "hi"));
  PollAndSettle("w1");

  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(events_[0].watch_id, "w1");
  ASSERT_EQ(events_[0].paths.size(), 1u);
  EXPECT_EQ(events_[0].paths[0], Path("new.txt").AsUTF8Unsafe());
  EXPECT_EQ(events_[0].kind, "created");
  EXPECT_FALSE(events_[0].overflow);

  // Nothing changed since: no further event.
  PollAndSettle("w1");
  EXPECT_EQ(events_.size(), 1u);
}

// A tree past the snapshot cap: the walk sees only the first N entries, so
// a new file early in enumeration order pushes another past the cut. That
// file is unvisited, not deleted — no phantom delete (or create); the guest
// is told once, by an overflow event, and real edits still come through.
TEST_F(NexusFsWatchCoreTest, APollWalkCutAtTheCapInventsNoDeletes) {
  for (const char* name : {"b.txt", "c.txt", "d.txt", "e.txt"}) {
    ASSERT_TRUE(base::WriteFile(Path(name), "x"));
  }
  core_->SetMaxPollEntriesForTesting(3);
  ASSERT_TRUE(core_->Add("w1", temp_dir_.GetPath(), false, true).ok);

  PollAndSettle("w1");
  ASSERT_EQ(events_.size(), 1u);
  EXPECT_TRUE(events_[0].overflow);
  EXPECT_EQ(events_[0].paths,
            std::vector<std::string>{temp_dir_.GetPath().AsUTF8Unsafe()});

  ASSERT_TRUE(base::WriteFile(Path("a.txt"), "x"));
  PollAndSettle("w1");
  EXPECT_EQ(events_.size(), 1u) << "a partial walk reported " +
                                       (events_.size() > 1 ? events_.back().kind
                                                           : std::string());

  for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt"}) {
    ASSERT_TRUE(base::WriteFile(Path(name), "longer"));
  }
  PollAndSettle("w1");
  ASSERT_EQ(events_.size(), 2u);
  EXPECT_EQ(events_[1].kind, "modified");
  EXPECT_FALSE(events_[1].overflow);
}

TEST_F(NexusFsWatchCoreTest, ABurstIsCoalescedIntoOneEventWithAMixedKind) {
  ASSERT_TRUE(base::WriteFile(Path("old.txt"), "1"));
  ASSERT_TRUE(core_->Add("w1", temp_dir_.GetPath(), false, true).ok);

  ASSERT_TRUE(base::WriteFile(Path("a.txt"), "a"));
  ASSERT_TRUE(base::DeleteFile(Path("old.txt")));
  // Two passes inside one debounce window still make one event.
  core_->PollNowForTesting("w1");
  ASSERT_TRUE(base::WriteFile(Path("b.txt"), "b"));
  PollAndSettle("w1");

  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(events_[0].paths.size(), 3u);
  EXPECT_EQ(events_[0].kind, "change");
}

TEST_F(NexusFsWatchCoreTest, RemovedWatchReportsNothing) {
  ASSERT_TRUE(core_->Add("w1", temp_dir_.GetPath(), false, true).ok);
  EXPECT_TRUE(core_->Remove("w1"));
  EXPECT_FALSE(core_->Remove("w1"));
  ASSERT_TRUE(base::WriteFile(Path("late.txt"), "x"));
  core_->PollNowForTesting("w1");  // unknown id: a no-op
  task_environment_.FastForwardBy(NexusFsWatchCore::kPollInterval * 2);
  EXPECT_TRUE(events_.empty());
  EXPECT_EQ(core_->size(), 0u);
}

TEST_F(NexusFsWatchCoreTest, DuplicateIdIsRefused) {
  ASSERT_TRUE(core_->Add("w1", temp_dir_.GetPath(), false, true).ok);
  EXPECT_FALSE(core_->Add("w1", temp_dir_.GetPath(), false, true).ok);
}

TEST_F(NexusFsWatchCoreTest, TheIntervalTimerPollsWithoutHelp) {
  ASSERT_TRUE(core_->Add("w1", temp_dir_.GetPath(), true, true).ok);
  ASSERT_TRUE(base::WriteFile(Path("timed.txt"), "x"));
  task_environment_.FastForwardBy(NexusFsWatchCore::kPollInterval +
                                  NexusFsWatchCore::kDebounce);
  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(events_[0].kind, "created");
}

TEST_F(NexusFsWatchCoreTest, AnOversizedBurstIsCappedAndFlagged) {
  ASSERT_TRUE(core_->Add("w1", temp_dir_.GetPath(), false, true).ok);
  const size_t count = NexusFsWatchCore::kMaxPathsPerEvent + 10;
  for (size_t i = 0; i < count; ++i) {
    ASSERT_TRUE(base::WriteFile(Path("f" + base::NumberToString(i)), "x"));
  }
  PollAndSettle("w1");
  ASSERT_EQ(events_.size(), 1u);
  EXPECT_EQ(events_[0].paths.size(), NexusFsWatchCore::kMaxPathsPerEvent);
  EXPECT_TRUE(events_[0].overflow);
}

// The native path starts on a real directory (the mode is the platform's
// call — a recursive watch may still fall back to polling).
TEST_F(NexusFsWatchCoreTest, NativeWatchStartsOnAnExistingDirectory) {
  const auto added = core_->Add("w1", temp_dir_.GetPath(), false, false);
  ASSERT_TRUE(added.ok);
  EXPECT_TRUE(added.mode == "native" || added.mode == "poll");
  EXPECT_TRUE(core_->Remove("w1"));
}

// Windows' recursive watch names only the watched directory. The burst is
// resolved against the baseline snapshot, so the guest still learns WHICH
// file changed.
TEST_F(NexusFsWatchCoreTest, ARootOnlyNativeNotificationNamesTheChangedFile) {
  const auto added = core_->Add("w1", temp_dir_.GetPath(), true, false);
  ASSERT_TRUE(added.ok);
  if (added.mode != "native") {
    GTEST_SKIP() << "no native watcher on this platform";
  }
  ASSERT_TRUE(base::WriteFile(Path("named.txt"), "x"));
  core_->SimulateNativeChangeForTesting("w1", temp_dir_.GetPath());
  task_environment_.FastForwardBy(NexusFsWatchCore::kDebounce);
  // A real notification may also have arrived; the last burst is ours or
  // carries the same resolution.
  ASSERT_FALSE(events_.empty());
  bool named = false;
  for (const FsWatchEvent& event : events_) {
    for (const std::string& path : event.paths) {
      named = named || path == Path("named.txt").AsUTF8Unsafe();
    }
  }
  EXPECT_TRUE(named);
}

}  // namespace
}  // namespace nexus
