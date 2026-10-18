#ifndef CUSTOM_BROWSER_COMMON_CUSTOM_BROWSER_SWITCHES_H_
#define CUSTOM_BROWSER_COMMON_CUSTOM_BROWSER_SWITCHES_H_

namespace custom_browser::switches {

// When present on the command line, the first window of a profile opens in a
// browser mode (pref_mode()) instead of the default kNexus startup window.
// Consumed in CustomBrowserView::InitializeCustomBrowser to launch into a
// normal (non-kNexus) window. (The taskbar Jump List no longer offers a "New
// browser window" task that emits this; it remains a supported command-line
// switch.)
inline constexpr char kBrowserMode[] = "browser-mode";

// Which console host backs a pseudoconsole child (F-31). The only value is
// `system`: skip the bundled conpty.dll next to chrome.dll and use the OS's
// ConPTY. Absent, the bundled host is tried first.
inline constexpr char kConsoleHost[] = "velloc-console-host";
inline constexpr char kConsoleHostSystem[] = "system";

}  // namespace custom_browser::switches

#endif  // CUSTOM_BROWSER_COMMON_CUSTOM_BROWSER_SWITCHES_H_
