// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_config_registry.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "url/gurl.h"

namespace nexus {

namespace {

std::optional<HostPlatform> ParsePlatform(const std::string& s) {
  if (s == "win32" || s == "windows") {
    return HostPlatform::kWindows;
  }
  if (s == "darwin" || s == "mac" || s == "macos") {
    return HostPlatform::kMac;
  }
  if (s == "linux") {
    return HostPlatform::kLinux;
  }
  return std::nullopt;
}

std::optional<HostArch> ParseArch(const std::string& s) {
  if (s == "x64" || s == "x86_64" || s == "amd64") {
    return HostArch::kX64;
  }
  if (s == "arm64" || s == "aarch64") {
    return HostArch::kArm64;
  }
  return std::nullopt;
}

std::optional<BinaryArchiveFormat> ParseFormat(const std::string& s) {
  if (s == "zip") {
    return BinaryArchiveFormat::kZip;
  }
  if (s == "tar.gz" || s == "tgz" || s == "targz") {
    return BinaryArchiveFormat::kTarGz;
  }
  return std::nullopt;
}

// A pinned download must be https (the SHA gate protects integrity, but
// requiring https avoids a trivially-MITM'd plaintext fetch) — EXCEPT
// plain http is allowed for loopback hosts (local dev servers / mirrors /
// e2e), which are the same machine and add no remote attack surface.
bool IsAllowedUrl(const std::string& url) {
  GURL gurl(url);
  if (!gurl.is_valid()) {
    return false;
  }
  if (gurl.SchemeIs("https")) {
    return true;
  }
  if (gurl.SchemeIs("http")) {
    const std::string_view host = gurl.host();
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]" ||
           host == "::1";
  }
  return false;
}

bool IsHex64(const std::string& s) {
  if (s.size() != 64) {
    return false;
  }
  for (char c : s) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}

std::string MakeKey(std::string_view id, HostPlatform platform, HostArch arch) {
  return base::StrCat({id, "|", base::NumberToString(static_cast<int>(platform)),
                       "|", base::NumberToString(static_cast<int>(arch))});
}

}  // namespace

NexusBinaryConfigRegistry& NexusBinaryConfigRegistry::Get() {
  static base::NoDestructor<NexusBinaryConfigRegistry> instance;
  return *instance;
}

NexusBinaryConfigRegistry::NexusBinaryConfigRegistry() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NexusBinaryConfigRegistry::~NexusBinaryConfigRegistry() = default;

size_t NexusBinaryConfigRegistry::SetFromDict(const base::DictValue& dict) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const base::ListValue* binaries = dict.FindList("binaries");
  if (!binaries) {
    return 0;  // Keep the previous good config.
  }

  std::map<std::string, ResolvedBinaryAsset, std::less<>> parsed;

  for (const base::Value& binary_value : *binaries) {
    const base::DictValue* binary = binary_value.GetIfDict();
    if (!binary) {
      continue;
    }
    const std::string* id = binary->FindString("id");
    if (!id || id->empty()) {
      continue;
    }
    const base::ListValue* assets = binary->FindList("assets");
    if (!assets) {
      continue;
    }
    for (const base::Value& asset_value : *assets) {
      const base::DictValue* asset = asset_value.GetIfDict();
      if (!asset) {
        continue;
      }
      const std::string* platform_s = asset->FindString("platform");
      const std::string* arch_s = asset->FindString("arch");
      const std::string* url = asset->FindString("url");
      const std::string* sha = asset->FindString("sha256");
      const std::string* format_s = asset->FindString("format");
      const std::string* member = asset->FindString("member");
      if (!platform_s || !arch_s || !url || !sha || !format_s || !member) {
        continue;
      }
      std::optional<HostPlatform> platform = ParsePlatform(*platform_s);
      std::optional<HostArch> arch = ParseArch(*arch_s);
      std::optional<BinaryArchiveFormat> format = ParseFormat(*format_s);
      if (!platform || !arch || !format) {
        continue;
      }
      if (!IsAllowedUrl(*url) || !IsHex64(*sha) || member->empty()) {
        continue;
      }
      parsed.insert_or_assign(
          MakeKey(*id, *platform, *arch),
          ResolvedBinaryAsset(*url, *sha, *format, *member));
    }
  }

  // Only replace if we got at least one valid asset — a malformed publish
  // must not wipe a working registry.
  const size_t accepted = parsed.size();
  if (accepted > 0) {
    assets_ = std::move(parsed);
  }
  return accepted;
}

std::optional<ResolvedBinaryAsset> NexusBinaryConfigRegistry::Resolve(
    std::string_view id,
    HostPlatform platform,
    HostArch arch) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = assets_.find(MakeKey(id, platform, arch));
  if (it == assets_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool NexusBinaryConfigRegistry::empty() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return assets_.empty();
}

void NexusBinaryConfigRegistry::ClearForTesting() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  assets_.clear();
}

}  // namespace nexus
