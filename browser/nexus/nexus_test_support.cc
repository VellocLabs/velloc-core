// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_test_support.h"

#include <memory>

#include "base/functional/bind.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/browser_context.h"

namespace nexus::test {
namespace {

std::unique_ptr<KeyedService> BuildNoService(content::BrowserContext*) {
  return nullptr;
}

}  // namespace

void AddGlicDisablingFactory(TestingProfile::Builder& builder) {
  builder.AddTestingFactory(glic::GlicKeyedServiceFactory::GetInstance(),
                            base::BindRepeating(&BuildNoService));
}

TestingProfile::TestingFactories GlicDisablingTestingFactories() {
  TestingProfile::TestingFactories factories;
  factories.emplace_back(glic::GlicKeyedServiceFactory::GetInstance(),
                         base::BindRepeating(&BuildNoService));
  return factories;
}

}  // namespace nexus::test
