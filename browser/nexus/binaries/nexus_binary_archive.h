// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_ARCHIVE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_ARCHIVE_H_

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"

namespace nexus {

// True when SHA-256(`bytes`) equals `expected_hex` (case-insensitive).
// This is the integrity gate that makes runtime download safe — callers
// MUST verify before writing/executing.
bool VerifyBinarySha256(base::span<const uint8_t> bytes,
                        std::string_view expected_hex);

// Extract a single member (by exact archive path, forward-slash
// separated) from an in-memory archive of the given format. Returns the
// member's decompressed bytes, or nullopt on any failure: bad archive,
// member missing, or a member that exceeds `max_member_bytes`.
//
// Pure + synchronous + CPU-bound — call this on a `base::ThreadPool`
// sequence, never the UI thread. zip uses `zip::ZipReader`, tar.gz uses
// `compression::GzipUncompress` + a minimal multi-entry ustar walk
// (Chromium's `SingleFileTarReader` only handles single-entry archives).
std::optional<std::vector<uint8_t>> ExtractArchiveMember(
    base::span<const uint8_t> archive,
    BinaryArchiveFormat format,
    std::string_view member,
    size_t max_member_bytes);

// Verify `archive`'s SHA-256 against `expected_sha256`, extract `member`,
// and atomically install it at `dest` (parent dirs created; written to a
// temp sibling then renamed, overwriting any existing file; POSIX exec bit
// set). Also writes a `<dest>.sha256` sidecar recording `expected_sha256`
// so a later IsInstalledUpToDate() can detect a config/version bump.
// Returns false — writing nothing at `dest` — on a hash mismatch,
// extraction failure, or any filesystem error.
//
// Blocking + CPU-bound (hashing + decompression + file I/O): call only on
// a base::ThreadPool MayBlock sequence, never the UI thread.
bool VerifyExtractAndWrite(base::span<const uint8_t> archive,
                           std::string_view expected_sha256,
                           BinaryArchiveFormat format,
                           std::string_view member,
                           const base::FilePath& dest,
                           size_t max_member_bytes);

// True when `dest` exists AND its `<dest>.sha256` sidecar records
// `expected_sha256` — i.e. the installed binary matches the currently
// pinned archive. A mismatch (or missing sidecar/binary) returns false so
// the installer re-downloads, which is how an online version bump (new
// pinned SHA) propagates to a machine that already has the old binary.
// Blocking I/O — ThreadPool only.
bool IsInstalledUpToDate(const base::FilePath& dest,
                         std::string_view expected_sha256);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_ARCHIVE_H_
