// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SECRETS_NEXUS_SECRET_STORE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SECRETS_NEXUS_SECRET_STORE_H_

#include <stddef.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"

class GURL;
class PrefService;

namespace url {
class Origin;
}

namespace nexus {

// ---- Pure rules (no prefs, no crypto) --------------------------------------

// A secret name: `<namespace>:<rest>`, namespace one of provider | plugin |
// mcp, rest 1-128 chars of [A-Za-z0-9_.:/@-]. The namespace says who owns
// it; nothing else in the name is interpreted.
bool IsValidNamespacedSecretName(std::string_view name);

// The namespace of a valid name ("provider" for "provider:openai").
std::string_view SecretNamespace(std::string_view name);

// The origin a secret may be bound to, serialized (`https://api.example.com`),
// or nullopt when `origin` is not acceptable: an absolute https URL, or http
// only for a loopback host (a local server the person runs). Path, query and
// fragment are ignored — the binding is to the origin.
std::optional<std::string> CanonicalSecretOrigin(std::string_view origin);

// Whether a secret in `name_space` may be marked for process use
// (`processEnv`): plugin and mcp — the owners of the stdio servers and
// children that read a credential from their environment. Provider keys only
// ever go to their provider's origin.
bool SecretNamespaceAllowsProcessEnv(std::string_view name_space);

// One placeholder's lookup, for a request to a given origin (or, for
// shell.spawn's env, for a child process).
struct SecretLookupResult {
  enum class Status {
    kFound,
    kMissing,
    kOriginMismatch,
    kUnavailable,
    // The secret exists but was not stored with processEnv, so it may not be
    // laid into a child's environment.
    kNotPermitted,
  };
  Status status = Status::kMissing;
  std::string value;
};
using NamespacedSecretLookup =
    base::RepeatingCallback<SecretLookupResult(const std::string& name)>;

// An error from expanding placeholders: a bridge code and a message.
struct SecretExpansionError {
  // SECRET_UNAVAILABLE | SECRET_ORIGIN_MISMATCH | SECRET_NOT_PERMITTED |
  // INVALID_ARGS
  std::string code;
  std::string message;
};

// Expands every `{{secret:NAME}}` in `text` into `out` (NAME namespaced, as
// above). Text without a placeholder is copied verbatim and `lookup` is not
// called. Returns the first error: a malformed placeholder or name
// (INVALID_ARGS), a secret that does not exist / cannot be decrypted
// (SECRET_UNAVAILABLE), one bound to another origin
// (SECRET_ORIGIN_MISMATCH), or one not marked for process use in a child's
// env (SECRET_NOT_PERMITTED) — the request / child is then not started.
std::optional<SecretExpansionError> ExpandSecretPlaceholders(
    std::string_view text,
    const NamespacedSecretLookup& lookup,
    std::string* out);

// True when `text` contains a placeholder at all.
bool HasSecretPlaceholder(std::string_view text);

// ---- The store -------------------------------------------------------------

// The `secrets` capability's storage: `prefs::kNexusSecrets`, sealed with
// NexusSecretCrypt. UI thread. Security: no Get — plaintext only goes into a
// request to its bound origin (Lookup) or a child's env (LookupForProcessEnv).
class NexusSecretStore {
 public:
  struct Entry {
    std::string name;
    std::string origin;  // Empty for a process-only secret.
    bool process_env = false;
  };

  static constexpr size_t kMaxSecrets = 512;
  static constexpr size_t kMaxValueBytes = 16 * 1024;

  explicit NexusSecretStore(PrefService* prefs);
  ~NexusSecretStore();

  NexusSecretStore(const NexusSecretStore&) = delete;
  NexusSecretStore& operator=(const NexusSecretStore&) = delete;

  // Stores (or replaces) `name`, bound to `origin`. `process_env` also lets
  // shell.spawn lay it into a child's env (plugin / mcp namespaces only);
  // with it, `origin` may be empty — a process-only secret no network request
  // can use. Replacing a secret without `process_env` clears the mark.
  // Refuses rather than ever storing plaintext: SECRET_STORE_UNAVAILABLE when
  // the OS keystore is not ready. Other refusals are INVALID_ARGS /
  // TOO_MANY_SECRETS.
  std::optional<SecretExpansionError> Set(const std::string& name,
                                          const std::string& value,
                                          const std::string& origin,
                                          bool process_env = false);
  // The bound origin when `name` exists (empty for a process-only secret).
  std::optional<std::string> Has(const std::string& name) const;
  // True when something was deleted.
  bool Delete(const std::string& name);
  // Sorted by name; `name_space` ("provider" / "plugin" / "mcp") filters.
  std::vector<Entry> ListNames(
      const std::optional<std::string>& name_space) const;

  // The plaintext of `name` for a request to `request_origin`.
  SecretLookupResult Lookup(const std::string& name,
                            const url::Origin& request_origin) const;

  // A lookup bound to `request_origin`, for ExpandSecretPlaceholders. The
  // store must outlive the callback's use (it is used synchronously).
  NamespacedSecretLookup LookupFor(const url::Origin& request_origin) const;

  // The plaintext of `name` for a child process's environment: kNotPermitted
  // unless it was stored with `process_env`. The origin binding does not
  // apply — a child is not a request.
  SecretLookupResult LookupForProcessEnv(const std::string& name) const;
  NamespacedSecretLookup ProcessEnvLookup() const;

 private:
  raw_ptr<PrefService> prefs_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SECRETS_NEXUS_SECRET_STORE_H_
