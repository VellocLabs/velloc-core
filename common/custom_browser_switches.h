#ifndef CUSTOM_BROWSER_COMMON_CUSTOM_BROWSER_SWITCHES_H_
#define CUSTOM_BROWSER_COMMON_CUSTOM_BROWSER_SWITCHES_H_

namespace custom_browser::switches {

// When present on the command line, the first window of a profile opens in a
// browser mode (pref_mode()) instead of the default kNexus startup window.
// Consumed in CustomBrowserView::InitializeCustomBrowser, and emitted by the
// taskbar Jump List "New browser window" task (nexus_jumplist_tasks.cc) to
// relaunch into a normal (non-kNexus) window.
inline constexpr char kBrowserMode[] = "browser-mode";

}  // namespace custom_browser::switches

#endif  // CUSTOM_BROWSER_COMMON_CUSTOM_BROWSER_SWITCHES_H_
