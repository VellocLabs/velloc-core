// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_WATCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_WATCHER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_path_watcher.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"

namespace nexus {

// ---- The poll fallback, as pure functions ----------------------------------
//
// A snapshot is what a poll compares: every path under the watched root (or
// the root itself when it is a file) with the facts a change moves.

struct FsSnapshotEntry {
  int64_t size = 0;
  double mtime_ms = 0;
  bool is_directory = false;

  bool operator==(const FsSnapshotEntry&) const = default;
};

using FsSnapshot = std::map<base::FilePath, FsSnapshotEntry>;

// Lists `root` (a file: itself; a directory: its children, every descendant
// when `recursive`) into a snapshot of at most `max_entries` rows. A missing
// root is an empty snapshot — a poll then sees it appear. `truncated` (when
// non-null) is set when the cap stopped the walk. Blocking.
FsSnapshot TakeFsSnapshot(const base::FilePath& root,
                          bool recursive,
                          size_t max_entries,
                          bool* truncated);

// What changed between two snapshots: (path, kind) with kind created |
// deleted | modified, ordered by path.
std::vector<std::pair<base::FilePath, std::string>> DiffFsSnapshots(
    const FsSnapshot& before,
    const FsSnapshot& after);

// The wire `kind` of one native change.
std::string FsChangeKindName(base::FilePathWatcher::ChangeType type);

// ---- The watch set ---------------------------------------------------------

// One coalesced burst of changes on one watch: the payload of the
// `fs.onChange` bridge event.
struct FsWatchEvent {
  FsWatchEvent();
  FsWatchEvent(const FsWatchEvent&);
  FsWatchEvent(FsWatchEvent&&);
  FsWatchEvent& operator=(const FsWatchEvent&);
  FsWatchEvent& operator=(FsWatchEvent&&);
  ~FsWatchEvent();

  std::string watch_id;
  std::vector<std::string> paths;  // UTF-8
  std::string kind;
  // More distinct paths changed in the burst than one event carries.
  bool overflow = false;
};

// Every watch one bridge connection holds, on one sequenced MayBlock runner
// (base::FilePathWatcher is sequence-bound). Destroying it cancels every
// watch. Changes are debounced per watch into one event per kDebounce burst.
class NexusFsWatchCore {
 public:
  using EventCallback = base::RepeatingCallback<void(FsWatchEvent)>;

  struct AddResult {
    bool ok = false;
    // "native" | "poll"
    std::string mode;
    std::string error;
  };

  static constexpr base::TimeDelta kDebounce = base::Milliseconds(150);
  static constexpr base::TimeDelta kPollInterval = base::Seconds(2);
  static constexpr size_t kMaxPathsPerEvent = 256;
  static constexpr size_t kMaxPollEntries = 20000;

  // `on_event` runs on this object's sequence; the owner binds it to post
  // to wherever the event is delivered.
  explicit NexusFsWatchCore(EventCallback on_event);
  ~NexusFsWatchCore();

  NexusFsWatchCore(const NexusFsWatchCore&) = delete;
  NexusFsWatchCore& operator=(const NexusFsWatchCore&) = delete;

  // Starts watching `path` under `watch_id` (unique per core; the owner mints
  // it). Falls back to polling when a recursive watch is unavailable on this
  // platform or the native watch cannot start. `force_poll` skips the native
  // watcher (tests, and filesystems known not to deliver notifications).
  AddResult Add(const std::string& watch_id,
                const base::FilePath& path,
                bool recursive,
                bool force_poll);

  // False when `watch_id` is unknown.
  bool Remove(const std::string& watch_id);

  size_t size() const { return watches_.size(); }

  // Runs one poll pass now instead of waiting for the interval (tests).
  void PollNowForTesting(const std::string& watch_id);
  // Lowers the snapshot cap (kMaxPollEntries) for watches added after.
  void SetMaxPollEntriesForTesting(size_t max_entries) {
    max_poll_entries_ = max_entries;
  }
  // Delivers a native notification for `path` as the platform would (tests:
  // the OS's own timing is not the test's).
  void SimulateNativeChangeForTesting(const std::string& watch_id,
                                      const base::FilePath& path);

 private:
  struct Watch;

  void StartPolling(const std::string& watch_id, Watch& watch);
  void SwitchToPolling(const std::string& watch_id);
  void OnNativeChange(const std::string& watch_id,
                      const base::FilePathWatcher::ChangeInfo& info,
                      const base::FilePath& path,
                      bool error);
  void OnPoll(const std::string& watch_id);
  void Note(const std::string& watch_id,
            Watch& watch,
            const base::FilePath& path,
            const std::string& kind);
  void Flush(const std::string& watch_id);

  size_t max_poll_entries_ = kMaxPollEntries;

  EventCallback on_event_;
  std::map<std::string, std::unique_ptr<Watch>> watches_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusFsWatchCore> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_FS_NEXUS_FS_WATCHER_H_
