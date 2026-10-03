// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"

#include <algorithm>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace nexus {

std::optional<std::string> CheckRawStreamTarget(const GURL& base_url,
                                                const GURL& target) {
  if (!base_url.is_valid() || !base_url.SchemeIsHTTPOrHTTPS()) {
    return "Provider has no valid http(s) base URL.";
  }
  if (!target.is_valid() || !target.SchemeIsHTTPOrHTTPS()) {
    return "Target must be an absolute http(s) URL.";
  }
  if (target.has_username() || target.has_password()) {
    return "Target URL may not embed credentials.";
  }
  if (target.scheme() != base_url.scheme()) {
    return "Target scheme must match the provider base URL's scheme.";
  }
  if (!net::registry_controlled_domains::SameDomainOrHost(
          base_url, target,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
    return "Target host is outside the provider's domain.";
  }
  return std::nullopt;
}

bool IsForbiddenRawStreamHeader(std::string_view name) {
  if (name.empty()) {
    return true;
  }
  static constexpr std::string_view kForbidden[] = {
      "cookie", "host", "content-length", "proxy-authorization"};
  for (std::string_view forbidden : kForbidden) {
    if (base::EqualsCaseInsensitiveASCII(name, forbidden)) {
      return true;
    }
  }
  return false;
}

namespace {

constexpr std::string_view kSecretOpen = "{{secret:";
constexpr std::string_view kSecretClose = "}}";

// RFC 6901: `~1` is `/`, `~0` is `~` (in that order).
std::string UnescapePointerToken(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  for (size_t i = 0; i < token.size(); ++i) {
    if (token[i] == '~' && i + 1 < token.size() &&
        (token[i + 1] == '0' || token[i + 1] == '1')) {
      out.push_back(token[i + 1] == '1' ? '/' : '~');
      ++i;
    } else {
      out.push_back(token[i]);
    }
  }
  return out;
}

// Resolves |pointer| inside |root|; nullptr when any step is missing.
base::Value* ResolvePointer(base::Value& root, std::string_view pointer) {
  if (pointer.empty() || pointer[0] != '/') {
    return nullptr;
  }
  base::Value* node = &root;
  for (std::string_view raw : base::SplitStringPiece(
           pointer.substr(1), "/", base::KEEP_WHITESPACE,
           base::SPLIT_WANT_ALL)) {
    const std::string token = UnescapePointerToken(raw);
    if (base::DictValue* dict = node->GetIfDict()) {
      node = dict->Find(token);
    } else if (base::ListValue* list = node->GetIfList()) {
      size_t index = 0;
      if (!base::StringToSizeT(token, &index) || index >= list->size()) {
        return nullptr;
      }
      node = &(*list)[index];
    } else {
      return nullptr;
    }
    if (!node) {
      return nullptr;
    }
  }
  return node;
}

}  // namespace

bool IsValidSecretName(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (char c : name) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '_' && c != '.' && c != '-') {
      return false;
    }
  }
  return true;
}

std::optional<std::string> ExpandSecretTemplates(std::string_view text,
                                                 const SecretLookup& lookup,
                                                 std::string* out) {
  out->clear();
  size_t pos = 0;
  while (true) {
    const size_t open = text.find(kSecretOpen, pos);
    if (open == std::string_view::npos) {
      out->append(text.substr(pos));
      return std::nullopt;
    }
    out->append(text.substr(pos, open - pos));
    const size_t name_start = open + kSecretOpen.size();
    const size_t close = text.find(kSecretClose, name_start);
    if (close == std::string_view::npos) {
      return "Unterminated {{secret:...}} placeholder.";
    }
    const std::string name(text.substr(name_start, close - name_start));
    if (!IsValidSecretName(name)) {
      return "Invalid secret name in placeholder: " + name;
    }
    std::optional<std::string> value = lookup.Run(name);
    if (!value || value->empty()) {
      return "Provider has no secret named " + name + ".";
    }
    out->append(*value);
    pos = close + kSecretClose.size();
  }
}

std::optional<std::string> ApplySecretCaptures(
    std::string_view body,
    const std::vector<SecretCapture>& captures,
    std::map<std::string, std::string>* captured) {
  std::optional<base::Value> root =
      base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!root) {
    return std::nullopt;
  }
  for (const SecretCapture& capture : captures) {
    base::Value* node = ResolvePointer(*root, capture.pointer);
    if (!node || !node->is_string() || node->GetString().empty()) {
      continue;
    }
    (*captured)[capture.secret] = node->GetString();
    *node = base::Value(kCapturedSecretRedaction);
  }
  std::string redacted;
  if (!base::JSONWriter::Write(*root, &redacted)) {
    return std::nullopt;
  }
  return redacted;
}

bool IsValidClientStreamId(std::string_view id) {
  if (id.size() < 8 || id.size() > 64) {
    return false;
  }
  for (char c : id) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

RecentIdSet::RecentIdSet() = default;
RecentIdSet::~RecentIdSet() = default;

void RecentIdSet::Insert(std::string_view id) {
  if (std::find(ids_.begin(), ids_.end(), id) != ids_.end()) {
    return;
  }
  if (ids_.size() >= kCapacity) {
    ids_.pop_front();
  }
  ids_.emplace_back(id);
}

bool RecentIdSet::Take(std::string_view id) {
  auto it = std::find(ids_.begin(), ids_.end(), id);
  if (it == ids_.end()) {
    return false;
  }
  ids_.erase(it);
  return true;
}

RawStreamChunkQueue::RawStreamChunkQueue() = default;
RawStreamChunkQueue::~RawStreamChunkQueue() = default;

void RawStreamChunkQueue::Append(std::string_view data) {
  if (data.empty()) {
    return;
  }
  chunks_.emplace_back(data);
  size_ += data.size();
}

std::string RawStreamChunkQueue::Take(size_t max_bytes) {
  std::string out;
  out.reserve(std::min(max_bytes, size_));
  while (!chunks_.empty() && out.size() < max_bytes) {
    const std::string& front = chunks_.front();
    const size_t available = front.size() - front_offset_;
    const size_t n = std::min(available, max_bytes - out.size());
    out.append(front, front_offset_, n);
    front_offset_ += n;
    size_ -= n;
    if (front_offset_ == front.size()) {
      chunks_.pop_front();
      front_offset_ = 0;
    }
  }
  return out;
}

}  // namespace nexus
