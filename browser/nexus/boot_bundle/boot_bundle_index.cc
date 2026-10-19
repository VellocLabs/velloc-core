// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_index.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "url/url_constants.h"

namespace nexus {

namespace {

constexpr int kFormat = 1;
constexpr char kDirectoryDocument[] = "index.html";

std::optional<BootBundleEntry> ParseEntry(const base::DictValue& file,
                                          uint64_t data_size) {
  const std::string* type = file.FindString("type");
  std::optional<double> offset = file.FindDouble("offset");
  std::optional<double> size = file.FindDouble("size");
  if (!type || type->empty() || !offset || !size || *offset < 0 ||
      *size < 0) {
    return std::nullopt;
  }
  BootBundleEntry entry;
  entry.offset = static_cast<uint64_t>(*offset);
  entry.size = static_cast<uint64_t>(*size);
  entry.mime_type = *type;
  if (entry.offset > data_size || entry.size > data_size - entry.offset) {
    return std::nullopt;
  }
  return entry;
}

std::string FallbackKey(const GURL& url) {
  return url.GetWithoutRef().spec();
}

}  // namespace

BootBundleIndex::BootBundleIndex() = default;
BootBundleIndex::BootBundleIndex(BootBundleIndex&&) = default;
BootBundleIndex& BootBundleIndex::operator=(BootBundleIndex&&) = default;
BootBundleIndex::~BootBundleIndex() = default;

// static
std::optional<BootBundleIndex> BootBundleIndex::Parse(std::string_view json,
                                                      uint64_t data_size) {
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root || root->FindInt("format") != kFormat) {
    return std::nullopt;
  }
  const std::string* base_path = root->FindString("base");
  std::optional<double> build_time_ms = root->FindDouble("buildTimeMs");
  const base::ListValue* origins = root->FindList("origins");
  const base::ListValue* files = root->FindList("files");
  if (!base_path || !build_time_ms || *build_time_ms < 0 || !origins || !files ||
      !base::StartsWith(*base_path, "/") || !base::EndsWith(*base_path, "/")) {
    return std::nullopt;
  }

  BootBundleIndex index;
  index.base_ = *base_path;
  index.build_time_ =
      base::Time::UnixEpoch() + base::Milliseconds(*build_time_ms);
  for (const base::Value& value : *origins) {
    if (!value.is_string()) {
      return std::nullopt;
    }
    GURL origin_url(value.GetString());
    if (!origin_url.is_valid() || !origin_url.SchemeIs(url::kHttpsScheme)) {
      return std::nullopt;
    }
    index.origins_.insert(url::Origin::Create(origin_url));
  }
  for (const base::Value& value : *files) {
    const base::DictValue* file = value.GetIfDict();
    if (!file) {
      return std::nullopt;
    }
    const std::string* path = file->FindString("path");
    std::optional<BootBundleEntry> entry = ParseEntry(*file, data_size);
    if (!path || path->empty() || base::StartsWith(*path, "/") || !entry) {
      return std::nullopt;
    }
    index.files_.insert_or_assign(*path, std::move(*entry));
  }
  if (const base::ListValue* fallback = root->FindList("fallback")) {
    for (const base::Value& value : *fallback) {
      const base::DictValue* file = value.GetIfDict();
      const std::string* url_string = file ? file->FindString("url") : nullptr;
      std::optional<BootBundleEntry> entry =
          file ? ParseEntry(*file, data_size) : std::nullopt;
      GURL url(url_string ? *url_string : std::string());
      if (!entry || !url.is_valid() || !url.SchemeIs(url::kHttpsScheme)) {
        return std::nullopt;
      }
      index.fallback_.insert_or_assign(FallbackKey(url), std::move(*entry));
    }
  }
  return index;
}

const BootBundleEntry* BootBundleIndex::Lookup(const GURL& url) const {
  if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme) ||
      !ServesOrigin(url::Origin::Create(url))) {
    return nullptr;
  }
  std::string_view path = url.path();
  if (!base::StartsWith(path, base_)) {
    return nullptr;
  }
  std::string_view relative = path.substr(base_.size());
  if (relative.empty()) {
    relative = kDirectoryDocument;
  }
  auto it = files_.find(relative);
  return it == files_.end() ? nullptr : &it->second;
}

const BootBundleEntry* BootBundleIndex::LookupFallback(const GURL& url) const {
  if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme)) {
    return nullptr;
  }
  auto it = fallback_.find(FallbackKey(url));
  return it == fallback_.end() ? nullptr : &it->second;
}

bool BootBundleIndex::ServesOrigin(const url::Origin& origin) const {
  return origins_.contains(origin);
}

}  // namespace nexus
