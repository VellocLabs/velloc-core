#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_APPEARANCE_NEXUS_APPEARANCE_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_APPEARANCE_NEXUS_APPEARANCE_BRIDGE_ADAPTER_H_

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
// The WRITE half of Settings · Appearance and the language row on Settings ·
// General. The read half is not here: the panel already resolves Chrome's live
// palette from chrome://theme/colors.css and posts it to the guest as
// `nexus-theme` (nexus_host_theme.ts), so a write through this adapter
// repaints the guest through machinery that already exists — ThemeService
// notifies, ColorChangeUpdater fires, the panel re-posts. `getAppearance`
// exists only so the controls can render their CURRENT position on mount.
//
// The two halves live in DIFFERENT pref stores, which is why this adapter
// keeps a Profile and still reaches for local state. The theme is
// per-PROFILE (ThemeService is profile-keyed), so the profile is held.
// `intl.app_locale` is NOT: outside ChromeOS it is registered only on the
// local-state registry (browser_process_impl.cc), so touching it through
// profile prefs CHECK-fails in PrefService and kills the browser process.
// Chrome writes it to local state on Windows too
// (LanguagesHandler::HandleSetProspectiveUILanguage).
//
// Threading: all actions run on UI — PrefService and ThemeService are both
// UI-thread-bound. Holds no per-call state.
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
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_APPEARANCE_NEXUS_APPEARANCE_BRIDGE_ADAPTER_H_
