// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_RAW_STREAM_POLICY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_RAW_STREAM_POLICY_H_

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"

class GURL;

namespace nexus {

// Pure helpers behind `model.rawStreamOpen` / `rawStreamRead` — the
// provider-scoped byte transport a guest-side protocol adapter drives (OpenAI
// / Anthropic / Responses JSON+SSE, Connect/protobuf, …). The host never
// parses a streamed body; these helpers decide WHERE a request may go, WHICH
// caller headers are refused, how secrets are substituted in and captured
// out, and how received bytes queue until the guest pulls them.

// Checks |target| against the provider's active |base_url|. Allowed when:
// both parse; |target| has no embedded credentials; the schemes match; and
// the two share a registrable domain (or are the same host) — so a provider
// whose API spans sibling hosts (`api2.example.com` for auth,
// `agent.us.example.com` for inference) works, but the injected credential
// never leaves the provider's domain. Returns an error message, or nullopt
// when allowed.
std::optional<std::string> CheckRawStreamTarget(const GURL& base_url,
                                                const GURL& target);

// True when a caller-supplied request header must be rejected: headers the
// guest must never set on a credentialed request (Cookie, Host,
// Content-Length, Proxy-Authorization; compared case-insensitively). Every
// other header — User-Agent and auth headers included — is the guest's.
bool IsForbiddenRawStreamHeader(std::string_view name);

// ---- Secret templates -----------------------------------------------------
//
// A request header value or query-parameter value may carry `{{secret:NAME}}`
// placeholders; the host substitutes the provider's stored secret NAME
// (`api_key` = the provider's API key; anything else = an entry of the
// provider config's `secrets`). The guest composes the full value
// (`Bearer {{secret:api_key}}`), so any auth scheme is expressible without a
// host change, and the secret itself never reaches the guest.

// Looks a secret up by name; nullopt when the provider has none by that name.
using SecretLookup =
    base::RepeatingCallback<std::optional<std::string>(const std::string&)>;

// A secret name: 1-64 chars of [A-Za-z0-9_.-].
bool IsValidSecretName(std::string_view name);

// Expands every `{{secret:NAME}}` in |text| into |out|. Returns an error
// message (and leaves |out| unspecified) on a malformed placeholder, an
// invalid name, or a secret the provider does not have. Text without
// placeholders is copied verbatim.
std::optional<std::string> ExpandSecretTemplates(std::string_view text,
                                                 const SecretLookup& lookup,
                                                 std::string* out);

// ---- OAuth templates -------------------------------------------------------
//
// For the OAuth-backed providers (chatgpt-pro, github-copilot, qwen-code) the
// credential is not a stored secret but a token the host refreshes or
// exchanges right before use (NexusOAuthManager::EnsureFreshToken). A header
// or query value may carry `{{oauth:access_token}}` (the backend-ready bearer:
// the refreshed ChatGPT / Qwen token, or the short-lived Copilot token minted
// from the GitHub one) and `{{oauth:account_id}}` (chatgpt-pro's
// `ChatGPT-Account-Id`). The host resolves them only after running that step,
// and they never come back to the guest.

struct OAuthValues {
  std::string access_token;
  std::string account_id;
};

// True when `text` carries an `{{oauth:` placeholder.
bool HasOAuthTemplate(std::string_view text);

// Expands every `{{oauth:NAME}}` in |text| into |out| (NAME: access_token |
// account_id). `values` null (the provider is not an OAuth provider, or the
// refresh failed) or an empty access_token is an error, as are an unknown NAME
// and a malformed placeholder. An empty account_id expands to "" (a ChatGPT
// sign-in may carry none) — see IsOmittedOAuthValue. Other text —
// `{{secret:…}}` included — is copied verbatim.
std::optional<std::string> ExpandOAuthTemplates(std::string_view text,
                                                const OAuthValues* values,
                                                std::string* out);

// True when a header / query value that carried an `{{oauth:…}}` placeholder
// expanded to nothing — `{{oauth:account_id}}` with no account id. The caller
// then omits the header / parameter rather than send it empty (the behaviour
// of the removed C++ chatgpt-pro provider, which set ChatGPT-Account-Id only
// when it had one). Advertised as the `model.rawStreamOpen.oauthOptional`
// primitive.
bool IsOmittedOAuthValue(std::string_view original, std::string_view expanded);

// ---- Secret capture -------------------------------------------------------
//
// A request may declare that fields of its JSON response are secrets (a
// login or token-refresh response). The host stores them and hands the guest
// the body with those fields redacted.

struct SecretCapture {
  // RFC 6901 JSON pointer into the response body, e.g. `/accessToken`.
  std::string pointer;
  // Secret name to store the value under (IsValidSecretName).
  std::string secret;
};

inline constexpr std::string_view kCapturedSecretRedaction = "[captured]";

// Parses |body| as JSON, moves each capture's string value into |captured|
// (secret name -> plaintext) and replaces it in the document with
// kCapturedSecretRedaction. A pointer that resolves to nothing, or to a
// non-string, captures nothing. Returns the redacted, re-serialized body, or
// nullopt when |body| is not JSON — the caller must then withhold the body
// (it may hold the secret in an unexpected shape).
std::optional<std::string> ApplySecretCaptures(
    std::string_view body,
    const std::vector<SecretCapture>& captures,
    std::map<std::string, std::string>* captured);

// ---- Caller-chosen stream ids ----------------------------------------------
//
// `rawStreamOpen` may name its own stream id, so the guest can cancel the
// request before the response head arrives (the id it would otherwise learn
// only from the head). See
// P-4.

// A caller-chosen stream id: 8-64 chars of [A-Za-z0-9_-].
bool IsValidClientStreamId(std::string_view id);

// Ids cancelled before the host saw their open. `rawStreamOpen` ships its
// body on a separate transferred port, so a `rawStreamCancel` sent right
// after it can be handled first; the open then finds its id here and is
// refused. FIFO-bounded: the oldest entry is evicted past kCapacity.
class RecentIdSet {
 public:
  static constexpr size_t kCapacity = 64;

  RecentIdSet();
  ~RecentIdSet();
  RecentIdSet(const RecentIdSet&) = delete;
  RecentIdSet& operator=(const RecentIdSet&) = delete;

  // No-op when |id| is already present.
  void Insert(std::string_view id);
  // Removes |id|; true when it was present.
  bool Take(std::string_view id);
  size_t size() const { return ids_.size(); }

 private:
  base::circular_deque<std::string> ids_;
};

// Byte queue between the network consumer and the guest's pulls. Appends
// are unbounded (the stream enforces the total-bytes cap); `Take` drains up
// to |max_bytes| from the front.
class RawStreamChunkQueue {
 public:
  RawStreamChunkQueue();
  ~RawStreamChunkQueue();
  RawStreamChunkQueue(const RawStreamChunkQueue&) = delete;
  RawStreamChunkQueue& operator=(const RawStreamChunkQueue&) = delete;

  void Append(std::string_view data);
  // Removes and returns up to |max_bytes| from the front, preserving order
  // across chunk boundaries (a partially drained chunk keeps its tail).
  std::string Take(size_t max_bytes);
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

 private:
  base::circular_deque<std::string> chunks_;
  // Bytes of chunks_.front() already handed out by a previous Take.
  size_t front_offset_ = 0;
  size_t size_ = 0;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_RAW_STREAM_POLICY_H_
