// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_TRACKER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_TRACKER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "chrome/browser/ui/browser_tab_strip_tracker.h"
#include "chrome/browser/ui/browser_tab_strip_tracker_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/tabs/public/tab_interface.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_counts.h"

class GURL;
class Profile;
class TabStripModel;

namespace content {
class WebContents;
}

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace nexus {

// One session-owned tab in the profile's kNexus window, as the guest's
// browser pane draws it. `id` is the tab handle's raw value, the id the
// browser tools use.
struct SessionTabEntry {
  int32_t id = 0;
  std::string title;
  std::string url;
  bool active = false;

  friend bool operator==(const SessionTabEntry&,
                         const SessionTabEntry&) = default;
};

// Profile-scoped, UI-thread registry of each Nexus session's tab counts and
// last work tab, visible across bridge ports. Fed by the tab-strip observer
// and by BrowserTool (`NotifyTabBound`, since the insert event precedes the
// tag); recomputes the whole map on each event and notifies changed sessions.
class NexusSessionTabTracker : public KeyedService,
                               public TabStripModelObserver,
                               public BrowserTabStripTrackerDelegate {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnSessionTabCountsChanged(const std::string& session_key,
                                           const SessionTabCounts& counts) = 0;
    // The session's kNexus-window tab list, in strip order: membership,
    // order, the active tab, a title or URL. Empty when the last one left.
    virtual void OnSessionTabsChanged(const std::string& session_key,
                                      const std::vector<SessionTabEntry>& tabs) {}
  };

  explicit NexusSessionTabTracker(Profile* profile);
  NexusSessionTabTracker(const NexusSessionTabTracker&) = delete;
  NexusSessionTabTracker& operator=(const NexusSessionTabTracker&) = delete;
  ~NexusSessionTabTracker() override;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Called by BrowserTool::BindTabToSession right after the tag attach —
  // see the class comment for why the strip-insert event isn't enough.
  void NotifyTabBound(content::WebContents* contents);

  // Mirror of BrowserTool::RecordWorkTab — the most recent tab a tool
  // actually dispatched against, per session.
  void SetLastWorkTab(const std::string& session_key, tabs::TabHandle handle);

  // The session's last work tab, validated alive + still carrying the
  // session's tag. Stale entries are pruned on read.
  std::optional<tabs::TabHandle> GetLastWorkTab(
      const std::string& session_key) const;

  SessionTabCounts GetCounts(const std::string& session_key) const;

  // Session-owned tabs hosted in the profile's kNexus window, in
  // browser-activation order.
  std::vector<tabs::TabHandle> CollectSessionTabsInNexusWindow(
      const std::string& session_key) const;

  // Brings one of the session's Nexus-window tabs to the foreground of the
  // Nexus window's tab strip: the last work tab when it is hosted there,
  // else the first Nexus-window session tab. Returns false (and activates
  // nothing — deliberately no focus steal) when the session has no tab in
  // the Nexus window, e.g. in separate-agent-window mode.
  bool ActivateSessionTab(const std::string& session_key);

  // The session's kNexus-window tabs in strip order, with the strip's
  // active tab marked.
  std::vector<SessionTabEntry> GetSessionTabs(
      const std::string& session_key) const;

  // Security: each acts only on a live kNexus-window tab that carries
  // `session_key`'s tag; any other id is refused (false, nothing done).
  bool ActivateTab(const std::string& session_key, int32_t tab_id);
  bool CloseTab(const std::string& session_key, int32_t tab_id);
  bool NavigateTab(const std::string& session_key,
                   int32_t tab_id,
                   const GURL& url);

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      int index,
                      TabChangeType change_type) override;

  // BrowserTabStripTrackerDelegate:
  bool ShouldTrackBrowser(BrowserWindowInterface* browser) override;

 private:
  std::vector<SessionTabSnapshotEntry> SnapshotTaggedTabs() const;
  void RecomputeAndNotify();
  // The kNexus-window strip index of the session's tab `tab_id`, or
  // kNoTab; `out_strip` receives the strip when found.
  int FindSessionTab(const std::string& session_key,
                     int32_t tab_id,
                     TabStripModel** out_strip) const;
  void NotifySessionTabs(const std::string& session_key);
  void NotifySessionTabsFor(content::WebContents* contents);

  const raw_ptr<Profile> profile_;
  SessionTabCountsMap counts_;
  base::flat_map<std::string, tabs::TabHandle> last_work_tabs_;
  base::ObserverList<Observer> observers_;
  // Last member so its Init-time OnTabStripModelChanged replays hit fully
  // constructed state.
  BrowserTabStripTracker strip_tracker_;
};

class NexusSessionTabTrackerFactory
    : public BrowserContextKeyedServiceFactory {
 public:
  static NexusSessionTabTracker* GetForProfile(Profile* profile);
  static NexusSessionTabTrackerFactory* GetInstance();

  NexusSessionTabTrackerFactory(const NexusSessionTabTrackerFactory&) = delete;
  NexusSessionTabTrackerFactory& operator=(
      const NexusSessionTabTrackerFactory&) = delete;

 private:
  friend class base::NoDestructor<NexusSessionTabTrackerFactory>;

  NexusSessionTabTrackerFactory();
  ~NexusSessionTabTrackerFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_TRACKER_H_
