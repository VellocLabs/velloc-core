// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_

#include <string>

#include "base/values.h"

class PrefService;

namespace nexus {

// Returns a pointer to the stored Dict for |provider_id| inside
// prefs::kNexusProviderConfigs, or nullptr if no entry exists or |prefs| is
// null. The returned pointer is owned by the PrefService and remains valid
// for the lifetime of the underlying stored dict; callers that need an
// independent copy should call ``Clone()``.
//
// Previously duplicated between nexus_llm_host.cc and
// nexus_settings_handler.cc; consolidated here so both read the same
// provider-config shape from prefs.
const base::DictValue* GetStoredProviderConfig(PrefService* prefs,
                                               const std::string& provider_id);

// Extracts the persisted default model id from a provider config Dict.
// Tries "default_model" first, falling back to the legacy "model" key.
// Returns an empty string if neither is present.
std::string GetStoredDefaultModelId(const base::DictValue& config);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDER_CONFIG_H_
