// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_archive.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "third_party/zlib/google/compression_utils.h"
#include "third_party/zlib/google/zip_reader.h"

namespace nexus {

namespace {

constexpr size_t kTarBlock = 512;

// ---- tar (ustar) helpers -------------------------------------------------

std::string ReadNulString(const std::string& tar, size_t off, size_t len) {
  size_t end = off;
  const size_t stop = std::min(off + len, tar.size());
  while (end < stop && tar[end] != '\0') {
    ++end;
  }
  return tar.substr(off, end - off);
}

std::optional<uint64_t> ReadOctal(const std::string& tar,
                                  size_t off,
                                  size_t len) {
  std::string raw = ReadNulString(tar, off, len);
  std::string_view s = base::TrimWhitespaceASCII(raw, base::TRIM_ALL);
  uint64_t value = 0;
  for (char c : s) {
    if (c < '0' || c > '7') {
      return std::nullopt;
    }
    value = value * 8 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

// The metadata sidecar recording which pinned archive `dest` came from.
base::FilePath SidecarPath(const base::FilePath& dest) {
  return dest.AddExtensionASCII(".sha256");
}

bool IsAllZero(const std::string& tar, size_t off, size_t len) {
  const size_t stop = std::min(off + len, tar.size());
  for (size_t i = off; i < stop; ++i) {
    if (tar[i] != '\0') {
      return false;
    }
  }
  return true;
}

// Walk a (decompressed) tar archive and return the named member's bytes.
// Supports the ustar prefix field for long paths. Symlinks/dirs/other
// typeflags are skipped; only regular files match.
std::optional<std::vector<uint8_t>> ExtractTarMember(const std::string& tar,
                                                     std::string_view member,
                                                     size_t max_member_bytes) {
  size_t offset = 0;
  while (offset + kTarBlock <= tar.size()) {
    if (IsAllZero(tar, offset, kTarBlock)) {
      break;  // end-of-archive padding
    }
    std::string name = ReadNulString(tar, offset, 100);
    if (ReadNulString(tar, offset + 257, 5) == "ustar") {
      std::string prefix = ReadNulString(tar, offset + 345, 155);
      if (!prefix.empty()) {
        name = prefix + "/" + name;
      }
    }
    std::optional<uint64_t> size = ReadOctal(tar, offset + 124, 12);
    if (!size.has_value()) {
      return std::nullopt;
    }
    const char typeflag = tar[offset + 156];
    const bool is_regular = (typeflag == '\0' || typeflag == '0');
    const size_t data_start = offset + kTarBlock;
    if (data_start + *size > tar.size()) {
      break;  // truncated entry
    }
    if (is_regular && std::string_view(name) == member) {
      if (*size > max_member_bytes) {
        return std::nullopt;
      }
      // Copy via substr (no raw pointer arithmetic — keeps the hardened
      // -Wunsafe-buffer-usage checker happy).
      std::string slice = tar.substr(data_start, static_cast<size_t>(*size));
      return std::vector<uint8_t>(slice.begin(), slice.end());
    }
    // Advance past the data, rounded up to the next 512-byte block.
    offset = data_start + ((*size + kTarBlock - 1) / kTarBlock) * kTarBlock;
  }
  return std::nullopt;
}

// ---- zip -----------------------------------------------------------------

std::optional<std::vector<uint8_t>> ExtractZipMember(
    base::span<const uint8_t> archive,
    std::string_view member,
    size_t max_member_bytes) {
  // Iterator construction avoids flagged raw-pointer + size access.
  std::string data(archive.begin(), archive.end());
  zip::ZipReader reader;
  if (!reader.OpenFromString(data)) {
    return std::nullopt;
  }
  while (const zip::ZipReader::Entry* entry = reader.Next()) {
    std::string path = entry->path.AsUTF8Unsafe();
    std::replace(path.begin(), path.end(), '\\', '/');
    if (std::string_view(path) != member) {
      continue;
    }
    std::string out;
    if (!reader.ExtractCurrentEntryToString(max_member_bytes, &out)) {
      return std::nullopt;
    }
    return std::vector<uint8_t>(out.begin(), out.end());
  }
  return std::nullopt;
}

}  // namespace

bool VerifyBinarySha256(base::span<const uint8_t> bytes,
                        std::string_view expected_hex) {
  std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(bytes);
  std::string actual = base::ToLowerASCII(base::HexEncode(digest));
  return actual.size() == expected_hex.size() &&
         base::EqualsCaseInsensitiveASCII(actual, expected_hex);
}

std::optional<std::vector<uint8_t>> ExtractArchiveMember(
    base::span<const uint8_t> archive,
    BinaryArchiveFormat format,
    std::string_view member,
    size_t max_member_bytes) {
  switch (format) {
    case BinaryArchiveFormat::kZip:
      return ExtractZipMember(archive, member, max_member_bytes);
    case BinaryArchiveFormat::kTarGz: {
      std::string tar;
      if (!compression::GzipUncompress(archive, &tar)) {
        return std::nullopt;
      }
      return ExtractTarMember(tar, member, max_member_bytes);
    }
  }
  return std::nullopt;
}

bool VerifyExtractAndWrite(base::span<const uint8_t> archive,
                           std::string_view expected_sha256,
                           BinaryArchiveFormat format,
                           std::string_view member,
                           const base::FilePath& dest,
                           size_t max_member_bytes) {
  if (!VerifyBinarySha256(archive, expected_sha256)) {
    return false;
  }
  std::optional<std::vector<uint8_t>> exe =
      ExtractArchiveMember(archive, format, member, max_member_bytes);
  if (!exe.has_value()) {
    return false;
  }
  if (!base::CreateDirectory(dest.DirName())) {
    return false;
  }
  // Write to a temp sibling then rename, so a partially written file is
  // never visible at `dest` as the installed binary.
  base::FilePath tmp = dest.AddExtensionASCII(".download");
  if (!base::WriteFile(tmp, *exe)) {
    base::DeleteFile(tmp);
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  // rwxr-xr-x — the resolver's --version probe (and exec) need the bit.
  if (!base::SetPosixFilePermissions(tmp, 0755)) {
    base::DeleteFile(tmp);
    return false;
  }
#endif
  if (!base::Move(tmp, dest)) {
    base::DeleteFile(tmp);
    return false;
  }
  // Record which pinned archive this is, so a later EnsureBinary can
  // re-download on a version bump. Best-effort: a missing/failed sidecar
  // just forces a re-download next time (IsInstalledUpToDate → false).
  base::WriteFile(SidecarPath(dest), expected_sha256);
  return true;
}

bool IsInstalledUpToDate(const base::FilePath& dest,
                         std::string_view expected_sha256) {
  if (!base::PathExists(dest)) {
    return false;
  }
  std::string recorded;
  if (!base::ReadFileToString(SidecarPath(dest), &recorded)) {
    return false;
  }
  return base::EqualsCaseInsensitiveASCII(
      base::TrimWhitespaceASCII(recorded, base::TRIM_ALL), expected_sha256);
}

}  // namespace nexus
