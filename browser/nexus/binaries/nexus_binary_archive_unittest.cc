// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_archive.h"

#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/compression_utils.h"
#include "third_party/zlib/google/zip.h"

namespace nexus {
namespace {

base::span<const uint8_t> AsBytes(const std::string& s) {
  return base::as_byte_span(s);
}

std::string ToString(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

// Build a single ustar header block (512 bytes) for a regular file.
std::string MakeTarHeader(const std::string& name, uint64_t size) {
  std::string h(512, '\0');
  h.replace(0, name.size(), name);                       // name[0..100)
  std::string oct = base::StringPrintf("%011o", static_cast<unsigned>(size));
  h.replace(124, oct.size(), oct);                       // size octal[124..136)
  h[156] = '0';                                          // typeflag: regular
  h.replace(257, 5, "ustar");                            // ustar magic[257..]
  return h;
}

// Build an in-memory (uncompressed) tar with a single regular-file entry.
std::string MakeTar(const std::string& name, const std::string& data) {
  std::string tar = MakeTarHeader(name, data.size());
  tar += data;
  size_t pad = (512 - (data.size() % 512)) % 512;
  tar.append(pad, '\0');
  tar.append(1024, '\0');  // two zero blocks = end of archive
  return tar;
}

// Build a real .zip (one file at `entry_path`) using Chromium's zip
// writer, returning its bytes — guaranteed valid (correct CRC, real
// deflate), which is what `zip::ZipReader` expects.
std::string MakeZip(const std::string& entry_path, const std::string& data) {
  base::ScopedTempDir tmp;
  CHECK(tmp.CreateUniqueTempDir());
  base::FilePath src = tmp.GetPath().AppendASCII("src");
  base::FilePath file = src.AppendASCII(entry_path);
  CHECK(base::CreateDirectory(file.DirName()));
  CHECK(base::WriteFile(file, data));
  base::FilePath dest = tmp.GetPath().AppendASCII("out.zip");
  CHECK(zip::Zip(src, dest, /*include_hidden_files=*/true));
  std::string bytes;
  CHECK(base::ReadFileToString(dest, &bytes));
  return bytes;
}

std::string Sha256Hex(const std::string& s) {
  return base::ToLowerASCII(base::HexEncode(crypto::hash::Sha256(AsBytes(s))));
}

constexpr size_t kMax = 64u * 1024u * 1024u;

// --- SHA-256 ---------------------------------------------------------------

TEST(NexusBinaryArchiveTest, VerifySha256MatchesKnownVector) {
  // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  const std::string abc = "abc";
  EXPECT_TRUE(VerifyBinarySha256(
      AsBytes(abc),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  // Case-insensitive on the expected hex.
  EXPECT_TRUE(VerifyBinarySha256(
      AsBytes(abc),
      "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
}

TEST(NexusBinaryArchiveTest, VerifySha256RejectsMismatch) {
  EXPECT_FALSE(VerifyBinarySha256(
      AsBytes("abc"),
      "0000000000000000000000000000000000000000000000000000000000000000"));
  EXPECT_FALSE(VerifyBinarySha256(AsBytes("abc"), "tooshort"));
}

// --- tar.gz ----------------------------------------------------------------

TEST(NexusBinaryArchiveTest, ExtractsTarGzMember) {
  const std::string payload = "#!/bin/sh\necho ripgrep\n";
  std::string tar = MakeTar("ripgrep-14.1.1-x86_64-unknown-linux-musl/rg",
                            payload);
  std::string gz;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(tar), &gz));

  auto out = ExtractArchiveMember(
      AsBytes(gz), BinaryArchiveFormat::kTarGz,
      "ripgrep-14.1.1-x86_64-unknown-linux-musl/rg", kMax);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(ToString(*out), payload);
}

TEST(NexusBinaryArchiveTest, TarGzMissingMemberReturnsNullopt) {
  std::string tar = MakeTar("pkg/other", "data");
  std::string gz;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(tar), &gz));
  EXPECT_FALSE(ExtractArchiveMember(AsBytes(gz), BinaryArchiveFormat::kTarGz,
                                    "pkg/rg", kMax)
                   .has_value());
}

TEST(NexusBinaryArchiveTest, TarGzMemberOverSizeCapReturnsNullopt) {
  const std::string payload = "0123456789";
  std::string tar = MakeTar("pkg/rg", payload);
  std::string gz;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(tar), &gz));
  EXPECT_FALSE(ExtractArchiveMember(AsBytes(gz), BinaryArchiveFormat::kTarGz,
                                    "pkg/rg", /*max_member_bytes=*/4)
                   .has_value());
}

TEST(NexusBinaryArchiveTest, RejectsCorruptGzip) {
  const std::string garbage = "not a gzip stream";
  EXPECT_FALSE(ExtractArchiveMember(AsBytes(garbage),
                                    BinaryArchiveFormat::kTarGz, "pkg/rg", kMax)
                   .has_value());
}

// --- zip -------------------------------------------------------------------

TEST(NexusBinaryArchiveTest, ExtractsZipMember) {
  const std::string payload = "MZ\x90\x00 fake-exe-bytes";
  std::string zip = MakeZip("ripgrep-14.1.1-x86_64-pc-windows-msvc/rg.exe",
                            payload);
  auto out = ExtractArchiveMember(
      AsBytes(zip), BinaryArchiveFormat::kZip,
      "ripgrep-14.1.1-x86_64-pc-windows-msvc/rg.exe", kMax);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(ToString(*out), payload);
}

TEST(NexusBinaryArchiveTest, ZipMissingMemberReturnsNullopt) {
  std::string zip = MakeZip("pkg/other.txt", "data");
  EXPECT_FALSE(ExtractArchiveMember(AsBytes(zip), BinaryArchiveFormat::kZip,
                                    "pkg/rg.exe", kMax)
                   .has_value());
}

TEST(NexusBinaryArchiveTest, RejectsCorruptZip) {
  const std::string garbage = "PK\x03\x04 not really a zip";
  EXPECT_FALSE(ExtractArchiveMember(AsBytes(garbage),
                                    BinaryArchiveFormat::kZip, "pkg/rg.exe",
                                    kMax)
                   .has_value());
}

// --- VerifyExtractAndWrite -------------------------------------------------

TEST(NexusBinaryArchiveTest, VerifyExtractAndWriteInstallsTarGzMember) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  const std::string payload = "rg-elf-bytes\n";
  std::string tar = MakeTar("pkg/rg", payload);
  std::string gz;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(tar), &gz));

  const base::FilePath dest = tmp.GetPath().AppendASCII("rg");
  EXPECT_TRUE(VerifyExtractAndWrite(AsBytes(gz), Sha256Hex(gz),
                                    BinaryArchiveFormat::kTarGz, "pkg/rg", dest,
                                    kMax));
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(dest, &written));
  EXPECT_EQ(written, payload);
#if BUILDFLAG(IS_POSIX)
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(dest, &mode));
  EXPECT_TRUE(mode & base::FILE_PERMISSION_EXECUTE_BY_USER);
#endif
}

TEST(NexusBinaryArchiveTest, VerifyExtractAndWriteInstallsZipMember) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  const std::string payload = "MZ-fake-windows-exe";
  std::string zip = MakeZip("pkg/rg.exe", payload);

  const base::FilePath dest = tmp.GetPath().AppendASCII("rg.exe");
  EXPECT_TRUE(VerifyExtractAndWrite(AsBytes(zip), Sha256Hex(zip),
                                    BinaryArchiveFormat::kZip, "pkg/rg.exe",
                                    dest, kMax));
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(dest, &written));
  EXPECT_EQ(written, payload);
}

TEST(NexusBinaryArchiveTest, VerifyExtractAndWriteRejectsBadShaAndWritesNothing) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  std::string tar = MakeTar("pkg/rg", "data");
  std::string gz;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(tar), &gz));

  const base::FilePath dest = tmp.GetPath().AppendASCII("rg");
  EXPECT_FALSE(VerifyExtractAndWrite(
      AsBytes(gz),
      "0000000000000000000000000000000000000000000000000000000000000000",
      BinaryArchiveFormat::kTarGz, "pkg/rg", dest, kMax));
  EXPECT_FALSE(base::PathExists(dest));
}

// --- sidecar / up-to-date (version-bump detection) -------------------------

TEST(NexusBinaryArchiveTest, RecordsSidecarAndReportsUpToDate) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  std::string gz;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(MakeTar("pkg/rg", "v1")), &gz));
  const std::string sha = Sha256Hex(gz);
  const base::FilePath dest = tmp.GetPath().AppendASCII("rg");

  ASSERT_TRUE(VerifyExtractAndWrite(AsBytes(gz), sha, BinaryArchiveFormat::kTarGz,
                                    "pkg/rg", dest, kMax));
  // Installed-from SHA matches the pinned SHA → up to date.
  EXPECT_TRUE(IsInstalledUpToDate(dest, sha));
  // A different pinned SHA (an online version bump) → NOT up to date, so
  // the installer would re-download.
  EXPECT_FALSE(IsInstalledUpToDate(
      dest,
      "1111111111111111111111111111111111111111111111111111111111111111"));
}

TEST(NexusBinaryArchiveTest, IsInstalledUpToDateFalseWhenBinaryOrSidecarMissing) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  const base::FilePath dest = tmp.GetPath().AppendASCII("rg");

  // No binary, no sidecar.
  EXPECT_FALSE(IsInstalledUpToDate(dest, Sha256Hex("x")));
  // Binary present but no sidecar (e.g. a manually-placed file) → false,
  // so we re-provision through the verified path.
  ASSERT_TRUE(base::WriteFile(dest, "manually placed"));
  EXPECT_FALSE(IsInstalledUpToDate(dest, Sha256Hex("x")));
}

TEST(NexusBinaryArchiveTest, OverwritesOldVersionOnReinstall) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  const base::FilePath dest = tmp.GetPath().AppendASCII("rg");

  std::string gz1;
  ASSERT_TRUE(compression::GzipCompress(AsBytes(MakeTar("pkg/rg", "OLD")), &gz1));
  ASSERT_TRUE(VerifyExtractAndWrite(AsBytes(gz1), Sha256Hex(gz1),
                                    BinaryArchiveFormat::kTarGz, "pkg/rg", dest,
                                    kMax));
  EXPECT_TRUE(IsInstalledUpToDate(dest, Sha256Hex(gz1)));

  // Re-install a NEW version (different bytes + SHA) over the old one.
  std::string gz2;
  ASSERT_TRUE(
      compression::GzipCompress(AsBytes(MakeTar("pkg/rg", "NEW-VERSION")), &gz2));
  ASSERT_TRUE(VerifyExtractAndWrite(AsBytes(gz2), Sha256Hex(gz2),
                                    BinaryArchiveFormat::kTarGz, "pkg/rg", dest,
                                    kMax));
  std::string written;
  ASSERT_TRUE(base::ReadFileToString(dest, &written));
  EXPECT_EQ(written, "NEW-VERSION");
  EXPECT_TRUE(IsInstalledUpToDate(dest, Sha256Hex(gz2)));
  EXPECT_FALSE(IsInstalledUpToDate(dest, Sha256Hex(gz1)));  // old SHA gone
}

}  // namespace
}  // namespace nexus
