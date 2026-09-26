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
//
// Per-client appearance (2026-09-20). The guest no longer reads the desktop's
// live palette: `resolveTheme` resolves a full colour theme for ONE client,
// parameterised by the request's (scheme, seed, variant), so a remote client
// on a different device OS / colour preference gets a palette independent of
// the desktop's own. ThemeService is used as a RESOLVER ONLY here —
// resolveTheme writes NO profile pref and sends NO broadcast; the result goes
// back only as the request's reply. The colour pipeline is the same one the
// legacy host-theme broadcast reads (ui::ColorProviderManager → ColorProvider,
// the pipeline feeding chrome://theme/colors.css), keyed by the request
// instead of the profile.
//
// `setColorScheme` / `setUserColor` are DEPRECATED but kept for ONE
// release: the /v1/ guest currently served (nexus_web main) still calls them
// from its Settings · Appearance page, so a binary without them breaks that
// page — and the one-version-served deploy model makes an interface-version
// bump useless here (the old guest is never served again). They behave exactly
// as they did before the per-client move (write the profile theme pref, the
// panel re-posts nexus-theme). Remove them once the per-client guest
// (resolveTheme + caller-localStorage) is the one being served.
// `setUiLocale` (unchanged) and `getAppearance` remain. getAppearance's
// three colour fields (scheme/variant/color) are kept OPTIONAL for one release
// so the desktop panel's one-shot migration can read the profile's previous
// values.
//
// `intl.app_locale` is NOT a profile pref: outside ChromeOS it is registered
// only on the local-state registry (browser_process_impl.cc), so touching it
// through profile prefs CHECK-fails in PrefService and kills the browser
// process. Chrome writes it to local state on Windows too
// (LanguagesHandler::HandleSetProspectiveUILanguage).
//
// Threading: all actions run on UI — PrefService, ThemeService and
// ColorProviderManager are all UI-thread-bound. Holds no per-call state.
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
