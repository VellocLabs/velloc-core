#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_BROKER_FACTORY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_BROKER_FACTORY_H_

#include "base/no_destructor.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"

class Profile;

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace nexus {

class LlmBroker;

// BrowserContext-keyed factory that creates a single LlmBroker per Profile.
class LlmBrokerFactory : public BrowserContextKeyedServiceFactory {
 public:
  static LlmBroker* GetForProfile(Profile* profile);
  static LlmBrokerFactory* GetInstance();

  LlmBrokerFactory(const LlmBrokerFactory&) = delete;
  LlmBrokerFactory& operator=(const LlmBrokerFactory&) = delete;

 private:
  friend class base::NoDestructor<LlmBrokerFactory>;

  LlmBrokerFactory();
  ~LlmBrokerFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
  // Instantiate the broker eagerly when the profile is created so the
  // catalogue refresh fires on every browser start, not just when the Nexus UI
  // is first opened. Without this the /v1/models fetch never runs for users
  // who have not yet interacted with the Nexus surface in the current
  // session, so the kNexusProviderCatalogs pref cache stays empty.
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_BROKER_FACTORY_H_
