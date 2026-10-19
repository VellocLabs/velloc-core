// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_store.h"

#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"

namespace nexus {

namespace {

constexpr char kIndexFileName[] = "nexus_boot_bundle.json";
constexpr char kDataFileName[] = "nexus_boot_bundle.dat";
// The index lists a few hundred files; anything larger is not ours.
constexpr size_t kMaxIndexBytes = 4 * 1024 * 1024;

BootBundleStore* g_store = nullptr;

std::optional<BootBundleIndex> LoadIndex(const base::FilePath& index_path,
                                         const base::FilePath& data_path) {
  std::optional<int64_t> data_size = base::GetFileSize(data_path);
  std::string json;
  if (!data_size || *data_size < 0 ||
      !base::ReadFileToStringWithMaxSize(index_path, &json, kMaxIndexBytes)) {
    return std::nullopt;
  }
  return BootBundleIndex::Parse(json, static_cast<uint64_t>(*data_size));
}

std::optional<std::string> ReadRange(const base::FilePath& data_path,
                                     uint64_t offset,
                                     uint64_t size) {
  base::File file(data_path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return std::nullopt;
  }
  std::string bytes(static_cast<size_t>(size), '\0');
  if (size > 0) {
    std::optional<size_t> read = file.Read(
        static_cast<int64_t>(offset), base::as_writable_byte_span(bytes));
    if (!read || *read != bytes.size()) {
      return std::nullopt;
    }
  }
  return bytes;
}

}  // namespace

// static
BootBundleStore* BootBundleStore::Get() {
  return g_store;
}

// static
void BootBundleStore::SetGlobal(BootBundleStore* store) {
  g_store = store;
}

BootBundleStore::BootBundleStore() = default;
BootBundleStore::~BootBundleStore() {
  if (g_store == this) {
    g_store = nullptr;
  }
}

void BootBundleStore::Load(const base::FilePath& dir) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  data_path_ = dir.AppendASCII(kDataFileName);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&LoadIndex, dir.AppendASCII(kIndexFileName), data_path_),
      base::BindOnce(&BootBundleStore::OnLoaded, weak_factory_.GetWeakPtr()));
}

void BootBundleStore::SetForTesting(BootBundleIndex index,
                                    const base::FilePath& data_path) {
  data_path_ = data_path;
  OnLoaded(std::move(index));
}

void BootBundleStore::OnLoaded(std::optional<BootBundleIndex> index) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (index && !index->empty()) {
    index_ = std::move(index);
  }
}

const BootBundleIndex* BootBundleStore::index() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return index_ ? &*index_ : nullptr;
}

void BootBundleStore::Read(const BootBundleEntry& entry,
                           ReadCallback callback) const {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ReadRange, data_path_, entry.offset, entry.size),
      std::move(callback));
}

}  // namespace nexus
