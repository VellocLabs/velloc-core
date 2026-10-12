#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_APPEARANCE_NEXUS_APPEARANCE_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_APPEARANCE_NEXUS_APPEARANCE_BRIDGE_ADAPTER_H_

#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/appearance/appearance_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `AppearanceApiBase` virtuals.
// `resolveTheme` resolves a colour theme for one client from the request's
// (scheme, seed, variant), writing no profile pref and sending no broadcast.
// `setColorScheme` / `setUserColor` are deprecated; the served guest still
// calls them. `intl.app_locale` lives in local state, not profile prefs.
// All actions run on UI. Holds no per-call state.
class NexusAppearanceBridgeAdapter
    : public ::nexus::generated::AppearanceApiBase {
 public:
  NexusAppearanceBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                               Profile* profile);
  ~NexusAppearanceBridgeAdapter() override;

  NexusAppearanceBridgeAdapter(const NexusAppearanceBridgeAdapter&) = delete;
  NexusAppearanceBridgeAdapter& operator=(const NexusAppearanceBridgeAdapter&) =
      delete;

 private:
  void HandleGetAppearanceOnUI(
      ::nexus::generated::GetAppearanceInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleResolveThemeOnUI(
      ::nexus::generated::ResolveThemeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleSetColorSchemeOnUI(
      ::nexus::generated::SetColorSchemeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleSetUserColorOnUI(
      ::nexus::generated::SetUserColorInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  void HandleSetUiLocaleOnUI(
      ::nexus::generated::SetUiLocaleInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<Profile> profile_;

  // Distinct resolveTheme keys this adapter has fed ColorProviderManager's
  // never-evicting cache; at kMaxDistinctResolvedThemes the cache is reset
  // so a client choosing keys cannot grow it without bound.
  static constexpr size_t kMaxDistinctResolvedThemes = 64;
  std::set<std::string> resolved_theme_keys_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_APPEARANCE_NEXUS_APPEARANCE_BRIDGE_ADAPTER_H_
