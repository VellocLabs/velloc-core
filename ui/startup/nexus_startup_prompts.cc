#include "custom_browser/ui/startup/nexus_startup_prompts.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/session_crashed_bubble.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/base/interaction/element_tracker.h"
#include "ui/views/interaction/element_tracker_views.h"

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

constexpr char kPendingAnchoredCrashBubbleKey[] =
    "custom_browser_pending_anchored_crash_bubble";

ui::ElementContext AnchorContextFor(BrowserWindowInterface* browser) {
  BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser);
  return view ? views::ElementTrackerViews::GetContextForView(view)
              : ui::ElementContext();
}

void ShowCrashBubbleNow(base::WeakPtr<BrowserWindowInterface> browser,
                        bool skip_tab_checking) {
  if (!browser) {
    return;
  }
  SessionCrashedBubble::ShowIfNotOffTheRecordProfile(browser.get(),
                                                     skip_tab_checking);
}

// Profile-owned wait for one window's popup anchor to become visible. At most
// one per profile; a newer request replaces an older one. If the window closes
// first, the wait stays idle until the profile is destroyed or replaced.
class PendingAnchoredCrashBubble : public base::SupportsUserData::Data {
 public:
  PendingAnchoredCrashBubble(Profile* profile,
                             BrowserWindowInterface* browser,
                             ui::ElementContext context,
                             bool skip_tab_checking)
      : profile_(profile),
        browser_(browser->GetWeakPtr()),
        skip_tab_checking_(skip_tab_checking) {
    subscription_ =
        ui::ElementTracker::GetElementTracker()->AddElementShownCallback(
            kFallbackPopupAnchorElementId, context,
            base::BindRepeating(&PendingAnchoredCrashBubble::OnAnchorShown,
                                base::Unretained(this)));
  }

 private:
  void OnAnchorShown(ui::TrackedElement* element) {
    // Posted: showing a bubble from inside the tracker's notification would
    // re-enter it.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ShowCrashBubbleNow, browser_, skip_tab_checking_));
    profile_->RemoveUserData(kPendingAnchoredCrashBubbleKey);  // Deletes this.
  }

  const raw_ptr<Profile> profile_;
  const base::WeakPtr<BrowserWindowInterface> browser_;
  const bool skip_tab_checking_;
  ui::ElementTracker::Subscription subscription_;
};

void ShowDeferredCrashBubbleNow(base::WeakPtr<BrowserWindowInterface> browser) {
  if (!browser) {
    return;
  }
  // skip_tab_checking=true: with `false`, SessionCrashedBubbleView::Show
  // silently drops the bubble when the window has no active tab yet — the
  // deferred restore offer must not be lost to startup-tab timing.
  ShowSessionCrashedBubbleWhenAnchored(browser.get(),
                                       /*skip_tab_checking=*/true);
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

void ShowSessionCrashedBubbleWhenAnchored(BrowserWindowInterface* browser,
                                          bool skip_tab_checking) {
  if (!browser || !browser->GetProfile()) {
    return;
  }
  Profile* profile = browser->GetProfile();
  const ui::ElementContext context = AnchorContextFor(browser);
  // Without a BrowserView the bubble has no app-menu anchor to fail on.
  if (!context || ui::ElementTracker::GetElementTracker()->IsElementVisible(
                      kFallbackPopupAnchorElementId, context)) {
    profile->RemoveUserData(kPendingAnchoredCrashBubbleKey);
    SessionCrashedBubble::ShowIfNotOffTheRecordProfile(browser,
                                                       skip_tab_checking);
    return;
  }
  profile->SetUserData(kPendingAnchoredCrashBubbleKey,
                       std::make_unique<PendingAnchoredCrashBubble>(
                           profile, browser, context, skip_tab_checking));
}

void MaybeShowDeferredCrashBubble(BrowserWindowInterface* browser) {
  if (!browser || !ConsumePendingDeferredCrashBubble(browser->GetProfile())) {
    return;
  }
  // Called from CustomBrowserView::InitializeCustomBrowser — inside
  // Widget::Init, before startup tabs exist — so post the show; the show
  // itself waits for the window's popup anchor.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&ShowDeferredCrashBubbleNow, browser->GetWeakPtr()));
}

}  // namespace custom_browser::nexus_startup_ui
