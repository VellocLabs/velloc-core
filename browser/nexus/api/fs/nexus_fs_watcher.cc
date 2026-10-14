// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_watcher.h"

#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"

namespace nexus {

namespace {

FsSnapshotEntry EntryFromInfo(int64_t size,
                              base::Time modified,
                              bool is_directory) {
  FsSnapshotEntry entry;
  entry.is_directory = is_directory;
  entry.size = is_directory ? 0 : size;
  entry.mtime_ms =
      modified.is_null() ? 0 : modified.InMillisecondsFSinceUnixEpoch();
  return entry;
}

}  // namespace

FsSnapshot TakeFsSnapshot(const base::FilePath& root,
                          bool recursive,
                          size_t max_entries,
                          bool* truncated) {
  if (truncated) {
    *truncated = false;
  }
  FsSnapshot snapshot;
  base::File::Info root_info;
  if (!base::GetFileInfo(root, &root_info)) {
    return snapshot;  // absent: a later poll sees it appear
  }
  if (!root_info.is_directory) {
    snapshot.emplace(root, EntryFromInfo(root_info.size,
                                         root_info.last_modified, false));
    return snapshot;
  }
  base::FileEnumerator enumerator(
      root, recursive,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  for (base::FilePath name = enumerator.Next(); !name.empty();
       name = enumerator.Next()) {
    if (snapshot.size() >= max_entries) {
      if (truncated) {
        *truncated = true;
      }
      break;
    }
    const base::FileEnumerator::FileInfo info = enumerator.GetInfo();
    snapshot.emplace(name, EntryFromInfo(info.GetSize(),
                                         info.GetLastModifiedTime(),
                                         info.IsDirectory()));
  }
  return snapshot;
}

std::vector<std::pair<base::FilePath, std::string>> DiffFsSnapshots(
    const FsSnapshot& before,
    const FsSnapshot& after) {
  std::vector<std::pair<base::FilePath, std::string>> changes;
  auto old_it = before.begin();
  auto new_it = after.begin();
  // Both maps are ordered by path: one merge pass.
  while (old_it != before.end() || new_it != after.end()) {
    if (new_it == after.end() ||
        (old_it != before.end() && old_it->first < new_it->first)) {
      changes.emplace_back(old_it->first, "deleted");
      ++old_it;
    } else if (old_it == before.end() || new_it->first < old_it->first) {
      changes.emplace_back(new_it->first, "created");
      ++new_it;
    } else {
      if (!(old_it->second == new_it->second)) {
        changes.emplace_back(new_it->first, "modified");
      }
      ++old_it;
      ++new_it;
    }
  }
  return changes;
}

std::string FsChangeKindName(base::FilePathWatcher::ChangeType type) {
  switch (type) {
    case base::FilePathWatcher::ChangeType::kCreated:
      return "created";
    case base::FilePathWatcher::ChangeType::kDeleted:
      return "deleted";
    case base::FilePathWatcher::ChangeType::kModified:
      return "modified";
    case base::FilePathWatcher::ChangeType::kMoved:
      return "moved";
    case base::FilePathWatcher::ChangeType::kUnknown:
      return "change";
  }
  return "change";
}

FsWatchEvent::FsWatchEvent() = default;
FsWatchEvent::FsWatchEvent(const FsWatchEvent&) = default;
FsWatchEvent::FsWatchEvent(FsWatchEvent&&) = default;
FsWatchEvent& FsWatchEvent::operator=(const FsWatchEvent&) = default;
FsWatchEvent& FsWatchEvent::operator=(FsWatchEvent&&) = default;
FsWatchEvent::~FsWatchEvent() = default;

struct NexusFsWatchCore::Watch {
  base::FilePath root;
  bool recursive = false;
  // Exactly one of these drives the watch.
  std::unique_ptr<base::FilePathWatcher> native;
  std::unique_ptr<base::RepeatingTimer> poll_timer;
  // Poll mode: the last pass. Native mode: the baseline a root-only
  // notification is resolved against (see Flush) — kept only while it is
  // complete (the walk was not cut at kMaxPollEntries).
  FsSnapshot snapshot;
  bool snapshot_complete = false;
  // Poll mode: the guest was told (by an overflow event) that the tree
  // outgrew the walk; told again only after a complete walk in between.
  bool reported_partial = false;

  // The burst being debounced.
  std::set<base::FilePath> pending;
  std::set<std::string> pending_kinds;
  bool overflow = false;
  base::OneShotTimer debounce;
};

NexusFsWatchCore::NexusFsWatchCore(EventCallback on_event)
    : on_event_(std::move(on_event)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NexusFsWatchCore::~NexusFsWatchCore() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

NexusFsWatchCore::AddResult NexusFsWatchCore::Add(const std::string& watch_id,
                                                  const base::FilePath& path,
                                                  bool recursive,
                                                  bool force_poll) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AddResult result;
  if (watch_id.empty() || watches_.contains(watch_id)) {
    result.error = "watch id in use";
    return result;
  }
  auto watch = std::make_unique<Watch>();
  watch->root = path;
  watch->recursive = recursive;

  bool native_ok = false;
  if (!force_poll &&
      (!recursive || base::FilePathWatcher::RecursiveWatchAvailable())) {
    auto native = std::make_unique<base::FilePathWatcher>();
    base::FilePathWatcher::WatchOptions options;
    options.type = recursive ? base::FilePathWatcher::Type::kRecursive
                             : base::FilePathWatcher::Type::kNonRecursive;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    // Name the changed file, not the watched directory, where the platform
    // can.
    options.report_modified_path = true;
#endif
    native_ok = native->WatchWithChangeInfo(
        path, options,
        base::BindRepeating(&NexusFsWatchCore::OnNativeChange,
                            weak_factory_.GetWeakPtr(), watch_id));
    if (native_ok) {
      watch->native = std::move(native);
    }
  }

  Watch& ref = *watch;
  watches_.emplace(watch_id, std::move(watch));
  if (native_ok) {
    // Some platforms (Windows' recursive watch) report only the WATCHED
    // path, not the descendant that changed; Flush resolves such a burst
    // against this baseline.
    bool truncated = false;
    ref.snapshot =
        TakeFsSnapshot(path, recursive, max_poll_entries_, &truncated);
    ref.snapshot_complete = !truncated;
    result.mode = "native";
  } else {
    StartPolling(watch_id, ref);
    result.mode = "poll";
  }
  result.ok = true;
  return result;
}

bool NexusFsWatchCore::Remove(const std::string& watch_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Destroying the Watch cancels its FilePathWatcher and both timers.
  return watches_.erase(watch_id) > 0;
}

void NexusFsWatchCore::PollNowForTesting(const std::string& watch_id) {
  OnPoll(watch_id);
}

void NexusFsWatchCore::SimulateNativeChangeForTesting(
    const std::string& watch_id,
    const base::FilePath& path) {
  OnNativeChange(watch_id, base::FilePathWatcher::ChangeInfo(), path, false);
}

void NexusFsWatchCore::StartPolling(const std::string& watch_id,
                                    Watch& watch) {
  watch.native.reset();
  bool truncated = false;
  watch.snapshot =
      TakeFsSnapshot(watch.root, watch.recursive, max_poll_entries_, &truncated);
  watch.snapshot_complete = !truncated;
  watch.poll_timer = std::make_unique<base::RepeatingTimer>();
  watch.poll_timer->Start(
      FROM_HERE, kPollInterval,
      base::BindRepeating(&NexusFsWatchCore::OnPoll,
                          weak_factory_.GetWeakPtr(), watch_id));
}

void NexusFsWatchCore::SwitchToPolling(const std::string& watch_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = watches_.find(watch_id);
  if (it != watches_.end() && !it->second->poll_timer) {
    StartPolling(watch_id, *it->second);
  }
}

void NexusFsWatchCore::OnNativeChange(
    const std::string& watch_id,
    const base::FilePathWatcher::ChangeInfo& info,
    const base::FilePath& path,
    bool error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = watches_.find(watch_id);
  if (it == watches_.end()) {
    return;
  }
  Watch& watch = *it->second;
  if (error) {
    // The native watcher gave up (and will not call again). Keep the
    // guest's watch alive on the poll path, and tell it something may have
    // changed in between.
    // Not inline: this runs inside the FilePathWatcher StartPolling would
    // destroy.
    Note(watch_id, watch, watch.root, "change");
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&NexusFsWatchCore::SwitchToPolling,
                                  weak_factory_.GetWeakPtr(), watch_id));
    return;
  }
  Note(watch_id, watch, path.empty() ? watch.root : path,
       FsChangeKindName(info.change_type));
}

void NexusFsWatchCore::OnPoll(const std::string& watch_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = watches_.find(watch_id);
  if (it == watches_.end()) {
    return;
  }
  Watch& watch = *it->second;
  bool truncated = false;
  FsSnapshot next = TakeFsSnapshot(watch.root, watch.recursive,
                                   max_poll_entries_, &truncated);
  // A walk cut at the cap saw part of the tree, and which part follows the
  // enumeration order: a path on one side only may just be unvisited, not
  // created or deleted. So from a partial walk only a change to a path both
  // walks saw is real; the rest is unwatched, and the guest is told once (an
  // overflow event on the root) to rescan rather than fed phantom deletes.
  const bool partial = truncated || !watch.snapshot_complete;
  for (const auto& [path, kind] : DiffFsSnapshots(watch.snapshot, next)) {
    if (!partial || kind == "modified") {
      Note(watch_id, watch, path, kind);
    }
  }
  if (truncated && !watch.reported_partial) {
    Note(watch_id, watch, watch.root, "change");
    watch.overflow = true;
  }
  watch.reported_partial = truncated;
  watch.snapshot = std::move(next);
  watch.snapshot_complete = !truncated;
}

void NexusFsWatchCore::Note(const std::string& watch_id,
                            Watch& watch,
                            const base::FilePath& path,
                            const std::string& kind) {
  if (watch.pending.size() < kMaxPathsPerEvent) {
    watch.pending.insert(path);
  } else if (!watch.pending.contains(path)) {
    watch.overflow = true;
  }
  watch.pending_kinds.insert(kind);
  if (!watch.debounce.IsRunning()) {
    watch.debounce.Start(
        FROM_HERE, kDebounce,
        base::BindOnce(&NexusFsWatchCore::Flush, weak_factory_.GetWeakPtr(),
                       watch_id));
  }
}

void NexusFsWatchCore::Flush(const std::string& watch_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = watches_.find(watch_id);
  if (it == watches_.end()) {
    return;
  }
  Watch& watch = *it->second;
  if (watch.pending.empty()) {
    return;
  }
  // A native burst that names only the watched directory (the platform could
  // not tell which descendant changed): name the changed paths from a fresh
  // snapshot instead. Kept as the root when the diff is empty or the tree is
  // too big to snapshot whole.
  if (watch.native && watch.snapshot_complete &&
      watch.pending.contains(watch.root)) {
    bool truncated = false;
    FsSnapshot next = TakeFsSnapshot(watch.root, watch.recursive,
                                     max_poll_entries_, &truncated);
    const auto changes = DiffFsSnapshots(watch.snapshot, next);
    watch.snapshot = std::move(next);
    watch.snapshot_complete = !truncated;
    if (!changes.empty() && watch.pending.size() == 1) {
      watch.pending.clear();
      watch.pending_kinds.clear();
      for (const auto& [path, kind] : changes) {
        if (watch.pending.size() < kMaxPathsPerEvent) {
          watch.pending.insert(path);
        } else {
          watch.overflow = true;
        }
        watch.pending_kinds.insert(kind);
      }
    }
  }
  FsWatchEvent event;
  event.watch_id = watch_id;
  event.paths.reserve(watch.pending.size());
  for (const base::FilePath& path : watch.pending) {
    event.paths.push_back(path.AsUTF8Unsafe());
  }
  event.kind = watch.pending_kinds.size() == 1 ? *watch.pending_kinds.begin()
                                               : std::string("change");
  event.overflow = watch.overflow;
  watch.pending.clear();
  watch.pending_kinds.clear();
  watch.overflow = false;
  on_event_.Run(std::move(event));
}

}  // namespace nexus
