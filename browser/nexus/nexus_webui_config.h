#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBUI_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBUI_CONFIG_H_

#include "build/buildflag.h"

namespace content {
class BrowserContext;
class WebUI;
class WebUIController;
}  // namespace content

namespace nexus {

// Registers the chrome://nexus WebUI host with the WebUI controller factory
// system. Should be called once at browser startup (typically from
// hooks_registration.cc via the Chromium Hooks system).
void RegisterNexusWebUI();

// Returns true if the chrome://nexus WebUI host has been registered.
bool IsNexusWebUIRegistered();

// The default guest URL served when no custom webapp URL is configured.
// Release builds get the production origin; Debug builds get the local Vite
// dev server.
extern const char kDefaultNexusGuestURL[];

// The production origin, defined in EVERY build config so tests can pin the
// shipped domain from a Debug build. `kDefaultNexusGuestURL` only carries it
// under NDEBUG, which would leave it unverified in the config developers
// build. Nothing reads this at runtime.
extern const char kReleaseNexusGuestURL[];

// The chrome://nexus host string.
extern const char kNexusHost[];

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_WEBUI_CONFIG_H_
