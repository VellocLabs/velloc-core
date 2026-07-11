#include "custom_browser/browser/nexus/llm/llm_broker_factory.h"

#include <utility>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/llm/llm_broker.h"

namespace nexus {

// static
LlmBroker* LlmBrokerFactory::GetForProfile(Profile* profile) {
  return static_cast<LlmBroker*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
LlmBrokerFactory* LlmBrokerFactory::GetInstance() {
  static base::NoDestructor<LlmBrokerFactory> instance;
  return instance.get();
}

LlmBrokerFactory::LlmBrokerFactory()
    : BrowserContextKeyedServiceFactory(
          "NexusLlmBroker",
          BrowserContextDependencyManager::GetInstance()) {}

LlmBrokerFactory::~LlmBrokerFactory() = default;

void LlmBrokerFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  LlmBroker::RegisterProfilePrefs(registry);
}

std::unique_ptr<KeyedService>
LlmBrokerFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  return std::make_unique<LlmBroker>(
      profile->GetPrefs(), profile->GetDefaultStoragePartition()
                               ->GetURLLoaderFactoryForBrowserProcess());
}

bool LlmBrokerFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

}  // namespace nexus
