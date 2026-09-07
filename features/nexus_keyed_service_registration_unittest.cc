// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/features/nexus_keyed_service_registration.h"

#include <set>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/dependency_graph.h"
#include "components/keyed_service/core/dependency_node.h"
#include "components/keyed_service/core/keyed_service_base_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// The names every Nexus BrowserContextKeyedServiceFactory has registered with
// the dependency manager so far (the ctor string each factory passes to
// BrowserContextKeyedServiceFactory).
std::set<std::string> RegisteredFactoryNames() {
  std::vector<raw_ptr<DependencyNode, VectorExperimental>> nodes;
  CHECK(BrowserContextDependencyManager::GetInstance()
            ->GetDependencyGraphForTesting()
            .GetConstructionOrder(&nodes));
  std::set<std::string> names;
  for (DependencyNode* node : nodes) {
    names.insert(static_cast<KeyedServiceBaseFactory*>(node)->name());
  }
  return names;
}

// Every Nexus keyed service must be reachable through this one call: a factory
// left out of it is not registered before
// DisallowKeyedServiceFactoryRegistration() runs, and DCHECK-fails the first
// time something reaches it lazily (dependency_manager.cc).
TEST(NexusKeyedServiceRegistrationTest, HookRegistersEveryNexusFactory) {
  NexusKeyedServiceLifecycleHook hook;
  hook.PreCreateThreads();

  const std::set<std::string> names = RegisteredFactoryNames();
  EXPECT_TRUE(names.contains("NexusLlmBroker"));
  EXPECT_TRUE(names.contains("NexusSessionTabTracker"));
  EXPECT_TRUE(names.contains("NexusBackgroundPanelHost"));
}

// The hook is built inside RegisterAllHooks(), which runs from
// CustomBrowserMainExtraParts' constructor — before BrowserProcess exists and
// before ExtensionsBrowserClient is set. Constructing a factory there crashes
// the browser at startup (NexusBackgroundPanelHostFactory DependsOn
// ThemeServiceFactory, which reaches ChromeExtensionSystemFactory's
// ExtensionsBrowserClient DCHECK), so building the hook must touch nothing:
// only PreCreateThreads() may register.
TEST(NexusKeyedServiceRegistrationTest, ConstructingTheHookRegistersNothing) {
  const std::set<std::string> before = RegisteredFactoryNames();
  NexusKeyedServiceLifecycleHook hook;
  EXPECT_EQ(before, RegisteredFactoryNames());
}

// Registration runs once per browser process today, but a second call must not
// double-register (a factory ctor is a NoDestructor singleton).
TEST(NexusKeyedServiceRegistrationTest, RegisteringTwiceIsANoOp) {
  RegisterNexusKeyedServiceFactories();
  const std::set<std::string> after_first = RegisteredFactoryNames();
  RegisterNexusKeyedServiceFactories();
  EXPECT_EQ(after_first, RegisteredFactoryNames());
}

}  // namespace
}  // namespace nexus
