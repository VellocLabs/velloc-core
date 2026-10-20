#include "custom_browser/ui/startup/nexus_startup_prompts.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/session_crashed_bubble.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"

namespace custom_browser::nexus_startup_ui {

namespace {

constexpr char kPendingDeferredCrashBubbleKey[] =
    "custom_browser_pending_deferred_crash_bubble";

// Transient marker attached to a Profile: the session-crashed bubble was
// suppressed on the kNexus startup window and should be offered on the next
// normal-type window. Never persisted — after a restart
// HasPendingUncleanExit() re-derives the state from the exit-type pref, so
// nothing double-shows.
class PendingDeferredCrashBubbleMarker : public base::SupportsUserData::Data {
};

void SetPendingDeferredCrashBubble(Profile* profile) {
  if (!profile) {
    return;
  }
  profile->SetUserData(kPendingDeferredCrashBubbleKey,
                       std::make_unique<PendingDeferredCrashBubbleMarker>());
}

bool ConsumePendingDeferredCrashBubble(Profile* profile) {
  if (!profile || !profile->GetUserData(kPendingDeferredCrashBubbleKey)) {
    return false;
  }
  profile->RemoveUserData(kPendingDeferredCrashBubbleKey);
  return true;
}

void ShowDeferredCrashBubbleNow(base::WeakPtr<BrowserWindowInterface> browser) {
  if (!browser) {
    return;
  }
  // skip_tab_checking=true: with `false`, SessionCrashedBubbleView::Show
  // silently drops the bubble when the window has no active tab yet — the
  // deferred restore offer must not be lost to startup-tab timing.
  SessionCrashedBubble::ShowIfNotOffTheRecordProfile(
      browser.get(), /*skip_tab_checking=*/true);
}

}  // namespace

bool MaybeSuppressStartupPrompts(BrowserWindowInterface* browser) {
  if (!browser) {
    return false;
  }
  BrowserWindowInterface* as_browser = browser;
  if (!custom_browser::window_util::IsNexusBrowser(as_browser)) {
    return false;
  }
  // The crash bubble's gate (HasPendingUncleanExit) burns the moment this
  // startup window finishes opening — stash the offer so the next normal
  // window can show it (MaybeShowDeferredCrashBubble).
  if (HasPendingUncleanExit(as_browser->GetProfile())) {
    SetPendingDeferredCrashBubble(as_browser->GetProfile());
  }
  return true;
}

void MaybeShowDeferredCrashBubble(BrowserWindowInterface* browser) {
  if (!browser || !ConsumePendingDeferredCrashBubble(browser->GetProfile())) {
    return;
  }
  // Called from CustomBrowserView::InitializeCustomBrowser — inside
  // Widget::Init, before startup tabs exist — so post the show; by the time
  // the task runs the window has finished coming up.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&ShowDeferredCrashBubbleNow, browser->GetWeakPtr()));
}

}  // namespace custom_browser::nexus_startup_ui
