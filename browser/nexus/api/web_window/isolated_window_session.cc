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
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
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

// Posted, never run inline: a closing browser still refers to its profile.
void PostDestroySession(Profile* session) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&DestroySession, session->GetWeakPtr()));
}

// Whether a browser of `session` is listed, one that is closing included.
bool HasWindow(Profile* session) {
  bool found = false;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [session, &found](BrowserWindowInterface* browser) {
        found = browser->GetProfile() == session;
        return !found;
      });
  return found;
}

// Owns itself: destroys the session after its last browser closed, or goes
// away with the session if that is destroyed first.
class SessionReaper : public BrowserCollectionObserver, public ProfileObserver {
 public:
  explicit SessionReaper(Profile* session) : session_(session) {
    browsers_.Observe(GlobalBrowserCollection::GetInstance());
    profile_.Observe(session);
  }

  // BrowserCollectionObserver:
  void OnBrowserClosed(BrowserWindowInterface* browser) override {
    if (browser->GetProfile() != session_ || HasWindow(session_)) {
      return;
    }
    PostDestroySession(session_);
    Stop();
  }

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override { Stop(); }

 private:
  ~SessionReaper() override = default;

  void Stop() {
    browsers_.Reset();
    profile_.Reset();
    delete this;
  }

  raw_ptr<Profile> session_;
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      browsers_{this};
  base::ScopedObservation<Profile, ProfileObserver> profile_{this};
};

}  // namespace

Profile* CreateIsolatedWindowSession(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  if (!IncognitoModePrefs::IsIncognitoAllowed(original)) {
    return nullptr;
  }
  // The DevTools kind of unique off-the-record profile is the one Chromium
  // lets open browser windows; nothing else here is about DevTools.
  return original->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUniqueForDevTools(),
      /*create_if_needed=*/true);
}

void DestroyIsolatedWindowSessionWhenUnused(Profile* session) {
  if (!session) {
    return;
  }
  if (!HasWindow(session)) {
    PostDestroySession(session);
    return;
  }
  new SessionReaper(session);
}

}  // namespace nexus
