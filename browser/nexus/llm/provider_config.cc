// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/llm/provider_config.h"

#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/llm/llm_pref_names.h"

namespace nexus {

const base::DictValue* GetStoredProviderConfig(
    PrefService* prefs,
    const std::string& provider_id) {
  if (!prefs) {
    return nullptr;
  }
  const base::DictValue& all_configs =
      prefs->GetDict(prefs::kNexusProviderConfigs);
  return all_configs.FindDict(provider_id);
}

std::string GetStoredDefaultModelId(const base::DictValue& config) {
  if (const auto* default_model = config.FindString("default_model");
      default_model && !default_model->empty()) {
    return *default_model;
  }
  if (const auto* model = config.FindString("model");
      model && !model->empty()) {
    return *model;
  }
  return std::string();
}

}  // namespace nexus
