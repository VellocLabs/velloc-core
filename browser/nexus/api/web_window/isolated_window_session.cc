// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/isolated_window_session.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"

namespace nexus {

namespace {

// How long the destroyer waits for the session's renderers to exit.
constexpr base::TimeDelta kDestroyTimeout = base::Seconds(60);

void DestroySession(base::WeakPtr<Profile> session) {
  if (Profile* profile = session.get()) {
    ProfileDestroyer::DestroyOTRProfileWhenAppropriateWithTimeout(
        profile, kDestroyTimeout);
  }
}

// Owns itself: waits for one browser to close, then destroys its session.
class SessionReaper : public BrowserCollectionObserver {
 public:
  SessionReaper(BrowserWindowInterface* browser, Profile* session)
      : browser_(browser), session_(session->GetWeakPtr()) {
    observation_.Observe(GlobalBrowserCollection::GetInstance());
  }

  // BrowserCollectionObserver:
  void OnBrowserClosed(BrowserWindowInterface* browser) override {
    if (browser != browser_) {
      return;
    }
    browser_ = nullptr;
    observation_.Reset();
    // Not here: the closing browser still refers to its profile.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&DestroySession, session_));
    delete this;
  }

 private:
  ~SessionReaper() override = default;

  raw_ptr<BrowserWindowInterface> browser_;
  base::WeakPtr<Profile> session_;
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      observation_{this};
};

}  // namespace

Profile* CreateIsolatedWindowSession(Profile* profile) {
  // The DevTools kind of unique off-the-record profile is the one Chromium
  // lets open browser windows; nothing else here is about DevTools.
  return profile->GetOriginalProfile()->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUniqueForDevTools(),
      /*create_if_needed=*/true);
}

void DestroyIsolatedWindowSessionAfter(BrowserWindowInterface* browser,
                                       Profile* session) {
  if (!session) {
    return;
  }
  if (!browser) {
    DestroySession(session->GetWeakPtr());
    return;
  }
  new SessionReaper(browser, session);
}

}  // namespace nexus
